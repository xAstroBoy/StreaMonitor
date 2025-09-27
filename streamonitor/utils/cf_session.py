# streamonitor/utils/cf_session.py
# Persistent async loop + sync wrapper for Bot compatibility.
# Buckets: 'api' (serialized), 'web' (moderate), 'hls' (fast).
# Robust network retry for transient curl errors (SSL reset, timeouts, etc.).
# Cookie minting only on CF HTML challenge. No global slowdowns for HLS.

import time
import random
import asyncio
import threading
from typing import Dict, List, Optional
from urllib.parse import urlparse

from parameters import DEBUG

from curl_cffi import requests as crequests
from curl_cffi import Cookies
from curl_cffi.requests.exceptions import (
    Timeout as CurlTimeout,
    ConnectionError as CurlConnectionError,
    SSLError as CurlSSLError,
    RequestException as CurlRequestError,
)

from .cf_broker import load_or_mint, mint_cookies_for, write


CF_HTML_SNIPPETS = (
    "<title>just a moment",
    "/cdn-cgi/challenge-platform/",
    "enable javascript and cookies to continue",
)

CDN_HOSTS = {"doppiocdn.com"}  # Stripchat HLS and segments


def _is_cf_html(text: str) -> bool:
    return bool(text) and any(s in text.lower() for s in CF_HTML_SNIPPETS)


def _is_cf_html_block(resp) -> bool:
    ct = (resp.headers.get("content-type") or "").lower()
    if "application/json" in ct:
        return False
    if resp.status_code in (403, 503, 429) and _is_cf_html(resp.text or ""):
        return True
    return _is_cf_html(resp.text or "")


def _infer_bucket(domain: str, path: str) -> str:
    p = (path or "").lower()
    if domain in CDN_HOSTS:
        return "hls"
    if p.startswith("/api/"):
        return "api"
    return "web"


# ---------- Background loop management ----------
_loop: Optional[asyncio.AbstractEventLoop] = None
_loop_thread: Optional[threading.Thread] = None


def _ensure_loop():
    global _loop, _loop_thread
    if _loop and _loop.is_running():
        return _loop

    loop = asyncio.new_event_loop()

    def run():
        asyncio.set_event_loop(loop)
        loop.run_forever()

    t = threading.Thread(target=run, daemon=True, name="CFSessionLoop")
    t.start()

    _loop = loop
    _loop_thread = t
    return _loop


def _run(coro):
    loop = _ensure_loop()
    return asyncio.run_coroutine_threadsafe(coro, loop).result()


# ---------- Host state ----------
class _HostState:
    def __init__(self, domain: str, visit_urls: List[str], profile: str,
                 impersonate: str = "chrome", logger=None, bot_id: str = ""):
        self.domain = domain
        self.visit_urls = visit_urls
        self.profile = profile
        self.bot_id = bot_id
        self.logger = logger

        # Async session. Keep defaults; we add per-call timeouts.
        self.sess = crequests.AsyncSession(impersonate=impersonate)

        # Profiles
        if profile == "api":
            self.req_lock = asyncio.Lock()  # serialize API calls
            self.capacity = 1
            self.refill_rate = 1.5          # ~1.5 req/s (per-process)
            self.backoff_base = 6.0
            self.backoff_cap = 45.0
        elif profile == "web":
            self.req_lock = None            # moderate, no hard serialization
            self.capacity = 3
            self.refill_rate = 2.0
            self.backoff_base = 4.0
            self.backoff_cap = 30.0
        else:  # hls (fast)
            self.req_lock = None            # NO serialization
            self.capacity = 12
            self.refill_rate = 30.0
            self.backoff_base = 2.0
            self.backoff_cap = 10.0

        # Token bucket
        self.tokens = float(self.capacity)
        self.last_refill = time.monotonic()

        # 429 shared backoff
        self.next_ok_time = 0.0
        self.backoff = self.backoff_base
        self._lock = asyncio.Lock()

        # CF cookie mint cooldown
        self.last_mint_ts = 0.0
        self.mint_cooldown = 20 * 60
        self.mint_lock = asyncio.Lock()

    def apply_cookie_data(self, data: dict):
        self.sess.headers.clear()
        self.sess.headers.update(data.get("headers", {}))

        jar = Cookies()
        for c in data.get("cookies", []):
            name = c.get("name")
            value = c.get("value")
            domain = c.get("domain") or ""
            path = c.get("path") or "/"
            try:
                jar.set(name=name, value=value, domain=domain, path=path)
            except TypeError:
                jar.set(name, value, domain)
            if domain.startswith("."):
                d2 = domain.lstrip(".")
                try:
                    jar.set(name=name, value=value, domain=d2, path=path)
                except TypeError:
                    jar.set(name, value, d2)
        self.sess.cookies = jar

    async def _refill(self):
        now = time.monotonic()
        elapsed = now - self.last_refill
        if elapsed > 0:
            self.tokens = min(self.capacity, self.tokens + elapsed * self.refill_rate)
            self.last_refill = now

    async def _acquire_slot(self):
        # Wait for backoff and token
        while True:
            async with self._lock:
                now = time.monotonic()
                wait_backoff = max(0.0, self.next_ok_time - now)
            if wait_backoff > 0:
                if self.logger and DEBUG:
                    self.logger.debug(f"{self.bot_id} [{self.domain}/{self.profile}] backoff {wait_backoff:.2f}s")
                await asyncio.sleep(min(wait_backoff, 0.05))
                continue

            async with self._lock:
                await self._refill()
                if self.tokens >= 1.0:
                    self.tokens -= 1.0
                    return
                need = (1.0 - self.tokens) / self.refill_rate
            await asyncio.sleep(min(need, 0.01))

    async def note_429(self, retry_after_header: Optional[str]):
        async with self._lock:
            now = time.monotonic()
            ra = 0.0
            if retry_after_header:
                try:
                    ra = float(retry_after_header)
                except Exception:
                    ra = 0.0
            if ra <= 0.0:
                ra = self.backoff
                self.backoff = min(self.backoff * 1.5, self.backoff_cap)
            else:
                self.backoff = self.backoff_base
            self.next_ok_time = max(self.next_ok_time, now + ra)
            if self.logger:
                self.logger.warning(f"{self.bot_id} [{self.domain}/{self.profile}] 429 → retry after {ra:.2f}s")

    async def note_success(self):
        async with self._lock:
            self.backoff = self.backoff_base
            self.next_ok_time = 0.0


# ---------- Robust request helper (retries on transient curl errors) ----------
async def _perform_with_retries(hs: _HostState, method: str, url: str, **kwargs):
    """
    Retries transient network failures:
      - SSLError (recv reset), ConnectionError, Timeout, RequestError
    Uses capped exponential backoff with jitter. Honors per-call timeout.
    """
    # Ensure a sane timeout per call (connect/read total)
    if "timeout" not in kwargs:
        # curl_cffi accepts float timeout (seconds)
        kwargs["timeout"] = 20.0

    # Some proxies/edges can be sensitive to HTTP/2. If you see repeated SSL resets,
    # you may experiment with forcing HTTP/1.1:
    # kwargs.setdefault("http_version", "1.1")

    last_exc = None
    for attempt in range(4):  # 1 + 3 retries
        try:
            return await hs.sess.request(method, url, **kwargs)
        except (CurlSSLError, CurlConnectionError, CurlTimeout, CurlRequestError) as e:
            last_exc = e
            # These are transient often (RST by peer, middleboxes, etc.)
            # Short sleep first, then exponential up to ~1s with jitter.
            base = min(0.1 * (2 ** attempt), 1.0)
            jitter = random.uniform(0.0, 0.2)
            if hs.logger and DEBUG:
                hs.logger.debug(
                    f"{hs.bot_id} [{hs.domain}/{hs.profile}] {type(e).__name__} on {method} {url} "
                    f"(attempt {attempt+1}/4) → sleep {base + jitter:.2f}s"
                )
            await asyncio.sleep(base + jitter)
            continue
    # Exhausted retries → raise original
    raise last_exc


# ---------- Manager ----------
class CFSessionManager:
    def __init__(self, max_age=6 * 3600, logger=None, bot_id: str = ""):
        self.max_age = max_age
        self._hosts: Dict[str, _HostState] = {}
        self._hosts_lock = asyncio.Lock()
        self.logger = logger
        self.bot_id = bot_id
        self._bootstrap_urls: Dict[str, List[str]] = {
            "stripchat.com": ["https://stripchat.com/", "https://hu.stripchat.com/"],
            "doppiocdn.com": ["https://stripchat.com/"],
        }

    async def _ensure_host(self, domain: str, bucket: str) -> _HostState:
        key = f"{domain}|{bucket}"
        async with self._hosts_lock:
            if key not in self._hosts:
                visit = self._bootstrap_urls.get(domain, [f"https://{domain}/"])
                hs = _HostState(domain, visit, profile=bucket,
                                logger=self.logger, bot_id=self.bot_id)
                # cf_broker functions are async in this build
                data = await load_or_mint(domain, visit, max_age=self.max_age)
                hs.apply_cookie_data(data)
                self._hosts[key] = hs
            return self._hosts[key]

    async def _request_async(self, method: str, url: str, **kwargs):
        # Debug: Check what we're actually getting

        
        # Additional safety check
        if not isinstance(url, str):
            error_msg = f"URL must be a string, got {type(url)}: {repr(url)}"
            if self.logger:
                self.logger.error(f"{self.bot_id} {error_msg}")
            raise TypeError(error_msg)
        
        try:
            parsed = urlparse(url)
        except Exception as e:
            print(f"DEBUG: urlparse failed with: {e}")
            print(f"DEBUG: url that failed: {repr(url)}")
            raise
            
        domain = parsed.hostname or "default"
        bucket = kwargs.pop("bucket", None) or _infer_bucket(domain, parsed.path or "/")
        hs = await self._ensure_host(domain, bucket)
        if self.logger and DEBUG:
            self.logger.debug(f"{self.bot_id} [{domain}/{bucket}] {method} {url}")

        # Pacing
        if bucket == "api" and hs.req_lock:
            async with hs.req_lock:
                await hs._acquire_slot()
                if random.random() < 0.25:
                    await asyncio.sleep(random.uniform(0.005, 0.02))
                r = await _perform_with_retries(hs, method, url, **kwargs)
        else:
            await hs._acquire_slot()
            r = await _perform_with_retries(hs, method, url, **kwargs)

        # CF HTML challenge → mint once then retry once
        if _is_cf_html_block(r):
            now = time.monotonic()
            if now - hs.last_mint_ts >= hs.mint_cooldown:
                async with hs.mint_lock:
                    now2 = time.monotonic()
                    if now2 - hs.last_mint_ts >= hs.mint_cooldown:
                        if self.logger:
                            self.logger.info(f"{self.bot_id} [{domain}/{bucket}] minting cookies")
                        data = await mint_cookies_for(domain, hs.visit_urls)
                        write(domain, data)
                        hs.apply_cookie_data(data)
                        hs.last_mint_ts = now2
                await asyncio.sleep(random.uniform(0.2, 0.5))
                r = await _perform_with_retries(hs, method, url, **kwargs)
            return r

        # 429 handling
        if r.status_code == 429:
            await hs.note_429(r.headers.get("Retry-After"))
            # One retry after partial wait; respects backoff in _acquire_slot
            await asyncio.sleep(min(1.0, hs.backoff / 2.0))
            await hs._acquire_slot()
            r2 = await _perform_with_retries(hs, method, url, **kwargs)
            if r2.status_code == 429:
                return r2
            await hs.note_success()
            return r2

        await hs.note_success()
        return r

    # --- sync wrappers for Bot ---
    def request(self, method: str, url_or_tuple, **kwargs):
        if isinstance(url_or_tuple, tuple):
            url = url_or_tuple[0]
        else:
            url = url_or_tuple
        url = str(url)
        return _run(self._request_async(method, url, **kwargs))

    def get(self, url_or_tuple, **kwargs):
        if isinstance(url_or_tuple, tuple):
            url = url_or_tuple[0]
        else:
            url = url_or_tuple
        url = str(url)
        return self.request("GET", url, **kwargs)

    def post(self, url_or_tuple, **kwargs):
        if isinstance(url_or_tuple, tuple):
            url = url_or_tuple[0]
        else:
            url = url_or_tuple
        url = str(url)
        return self.request("POST", url, **kwargs)

    # --- async API if needed ---
    async def arequest(self, method: str, url_or_tuple, **kwargs):
        if isinstance(url_or_tuple, tuple):
            url = url_or_tuple[0]
        else:
            url = url_or_tuple
        url = str(url)
        return await self._request_async(method, url, **kwargs)

    async def aget(self, url_or_tuple, **kwargs):
        if isinstance(url_or_tuple, tuple):
            url = url_or_tuple[0]
        else:
            url = url_or_tuple
        url = str(url)
        return await self._request_async("GET", url, **kwargs)

    async def apost(self, url_or_tuple, **kwargs):
        if isinstance(url_or_tuple, tuple):
            url = url_or_tuple[0]
        else:
            url = url_or_tuple
        url = str(url)
        return await self._request_async("POST", url, **kwargs)



