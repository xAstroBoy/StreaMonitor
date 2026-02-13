# streamonitor/utils/cf_session.py
# Persistent async loop + sync wrapper for Bot compatibility.
# Buckets: 'api' (serialized), 'web' (moderate), 'hls' (fast).
# Robust network retry for transient curl errors (SSL reset, timeouts, etc.).

import time
import random
import asyncio
import threading
from typing import Dict, List, Optional, Union
from urllib.parse import urlparse

import parameters

from curl_cffi import requests as crequests
from curl_cffi import Cookies
from curl_cffi.requests.exceptions import (
    Timeout as CurlTimeout,
    ConnectionError as CurlConnectionError,
    SSLError as CurlSSLError,
    RequestException as CurlRequestError,
    HTTPError as CurlHTTPError,
)

from .cf_broker import load_or_mint, mint_cookies_for, write
from .CloudflareDetection import looks_like_cf_html


CDN_HOSTS = {"doppiocdn.com", "doppiocdn.org", "doppiocdn.net"}  # Stripchat HLS


def _is_cf_html_block(resp) -> bool:
    """Check if response is a Cloudflare challenge page."""
    ct = (resp.headers.get("content-type") or "").lower()
    if "application/json" in ct:
        return False
    if resp.status_code in (403, 503, 429) and looks_like_cf_html(resp.text or ""):
        return True
    return looks_like_cf_html(resp.text or "")


def _infer_bucket(domain: str, path: str) -> str:
    """Determine rate limit bucket based on domain and path."""
    p = (path or "").lower()
    if domain in CDN_HOSTS:
        return "hls"
    if p.startswith("/api/") or "/api/" in p:
        return "api"
    return "web"


# ---------- Background loop management ----------
_loop: Optional[asyncio.AbstractEventLoop] = None
_loop_thread: Optional[threading.Thread] = None
_loop_lock = threading.Lock()


def _ensure_loop():
    """Ensure background asyncio event loop is running."""
    global _loop, _loop_thread
    
    with _loop_lock:
        if _loop and _loop.is_running():
            return _loop

        loop = asyncio.new_event_loop()

        def run():
            asyncio.set_event_loop(loop)
            try:
                loop.run_forever()
            finally:
                loop.close()

        t = threading.Thread(target=run, daemon=True, name="CFSessionLoop")
        t.start()

        _loop = loop
        _loop_thread = t
        return _loop


def _run(coro):
    """Run a coroutine in the background loop and wait for result."""
    loop = _ensure_loop()
    future = asyncio.run_coroutine_threadsafe(coro, loop)
    return future.result(timeout=120)  # 2 minute max wait


# ---------- Host state ----------
class _HostState:
    """Manages per-host rate limiting, backoff, and session state."""
    
    def __init__(self, domain: str, visit_urls: List[str], profile: str,
                 impersonate: str = "chrome", logger=None, bot_id: str = ""):
        self.domain = domain
        self.visit_urls = visit_urls
        self.profile = profile
        self.bot_id = bot_id
        self.logger = logger

        # Async session with reasonable defaults
        self.sess = crequests.AsyncSession(
            impersonate=impersonate,
            timeout=30.0  # Default timeout
        )

        # Profile-specific settings
        if profile == "api":
            self.req_lock = asyncio.Lock()  # Serialize API calls
            self.capacity = 1
            self.refill_rate = 1.5          # ~1.5 req/s
            self.backoff_base = 6.0
            self.backoff_cap = 45.0
        elif profile == "web":
            self.req_lock = None
            self.capacity = 3
            self.refill_rate = 2.0
            self.backoff_base = 4.0
            self.backoff_cap = 30.0
        else:  # hls (fast)
            self.req_lock = None
            self.capacity = 12
            self.refill_rate = 30.0
            self.backoff_base = 2.0
            self.backoff_cap = 10.0

        # Token bucket for rate limiting
        self.tokens = float(self.capacity)
        self.last_refill = time.monotonic()

        # 429 shared backoff state
        self.next_ok_time = 0.0
        self.backoff = self.backoff_base
        self._lock = asyncio.Lock()

        # CF cookie mint cooldown
        self.last_mint_ts = 0.0
        self.mint_cooldown = 20 * 60  # 20 minutes
        self.mint_lock = asyncio.Lock()

    async def _reset_session(self):
        """Reset the HTTP session to avoid connection reuse issues (e.g., HTTP/2 stream errors)."""
        try:
            await self.sess.close()
        except Exception:
            pass
        
        # Create a fresh session
        self.sess = crequests.AsyncSession(
            impersonate="chrome",
            timeout=30.0
        )
        
        # Reapply headers and cookies to the new session
        # (Assuming we have stored them, otherwise they'll be minted fresh on next request)

    def apply_cookie_data(self, data: dict):
        """Apply cookies and headers from broker data."""
        self.sess.headers.clear()
        self.sess.headers.update(data.get("headers", {}))

        jar = Cookies()
        for c in data.get("cookies", []):
            name = c.get("name")
            value = c.get("value")
            domain = c.get("domain") or ""
            path = c.get("path") or "/"
            
            # Handle both formats
            try:
                jar.set(name=name, value=value, domain=domain, path=path)
            except TypeError:
                jar.set(name, value, domain)
            
            # Also set for domain without leading dot
            if domain.startswith("."):
                d2 = domain.lstrip(".")
                try:
                    jar.set(name=name, value=value, domain=d2, path=path)
                except TypeError:
                    jar.set(name, value, d2)
        
        self.sess.cookies = jar

    async def _refill(self):
        """Refill token bucket based on elapsed time."""
        now = time.monotonic()
        elapsed = now - self.last_refill
        if elapsed > 0:
            self.tokens = min(self.capacity, self.tokens + elapsed * self.refill_rate)
            self.last_refill = now

    async def _acquire_slot(self):
        """Wait for rate limit slot to be available."""
        while True:
            # Check backoff
            async with self._lock:
                now = time.monotonic()
                wait_backoff = max(0.0, self.next_ok_time - now)
            
            if wait_backoff > 0:
                if self.logger and parameters.DEBUG:
                    self.logger.debug(
                        f"{self.bot_id} [{self.domain}/{self.profile}] "
                        f"backoff {wait_backoff:.2f}s"
                    )
                await asyncio.sleep(min(wait_backoff, 0.1))
                continue

            # Check token availability
            async with self._lock:
                await self._refill()
                if self.tokens >= 1.0:
                    self.tokens -= 1.0
                    return
                need = (1.0 - self.tokens) / self.refill_rate
            
            await asyncio.sleep(min(need, 0.05))

    async def note_429(self, retry_after_header: Optional[str]):
        """Record 429 rate limit and set backoff."""
        async with self._lock:
            now = time.monotonic()
            ra = 0.0
            
            if retry_after_header:
                try:
                    ra = float(retry_after_header)
                except (ValueError, TypeError):
                    ra = 0.0
            
            if ra <= 0.0:
                ra = self.backoff
                self.backoff = min(self.backoff * 1.5, self.backoff_cap)
            else:
                self.backoff = self.backoff_base
            
            self.next_ok_time = max(self.next_ok_time, now + ra)
            
            if self.logger:
                self.logger.warning(
                    f"{self.bot_id} [{self.domain}/{self.profile}] "
                    f"429 rate limit → retry after {ra:.2f}s"
                )

    async def note_success(self):
        """Record successful request and reset backoff."""
        async with self._lock:
            self.backoff = self.backoff_base
            self.next_ok_time = 0.0


# ---------- Robust request with retries ----------
async def _perform_with_retries(hs: _HostState, method: str, url: str, **kwargs):
    """
    Perform request with retries on transient network errors.
    Retries: SSLError, ConnectionError, Timeout, RequestError, HTTPError, HTTP/2 stream errors
    """
    # Ensure reasonable timeout
    if "timeout" not in kwargs:
        kwargs["timeout"] = 30.0

    last_exc = None
    for attempt in range(5):  # 1 initial + 4 retries for extra resilience on HTTP/2
        try:
            # Always reset session on HTTP/2 errors (not just on retry)
            # This is more aggressive but necessary for HTTP/2 stability
            if attempt > 0:
                # Force session renewal on retry to avoid stale HTTP/2 connections
                await hs._reset_session()
                # Extra delay for HTTP/2 errors to ensure connection is fully closed
                await asyncio.sleep(0.1)
            
            return await hs.sess.request(method, url, **kwargs)
        except (CurlSSLError, CurlConnectionError, CurlTimeout, CurlRequestError, CurlHTTPError) as e:
            last_exc = e
            error_str = str(e).lower()
            
            # Check if this is an HTTP/2 stream error (error 92)
            is_http2_error = "stream" in error_str and ("internal_error" in error_str or "err 2" in error_str)
            
            # Exponential backoff with jitter
            base = min(0.1 * (2 ** attempt), 1.5)
            jitter = random.uniform(0.0, 0.3)
            
            # Much longer backoff for HTTP/2 errors - these are serious
            if is_http2_error:
                base = min(1.0 * (2 ** attempt), 5.0)  # 1s -> 2s -> 4s -> 8s (capped at 5s)
                # Force immediate session reset on HTTP/2 errors
                try:
                    await hs._reset_session()
                    await asyncio.sleep(0.2)
                except Exception:
                    pass
            
            if hs.logger and parameters.DEBUG:
                hs.logger.debug(
                    f"{hs.bot_id} [{hs.domain}/{hs.profile}] "
                    f"{type(e).__name__} on {method} {url[:80]} "
                    f"(attempt {attempt + 1}/5) → sleep {base + jitter:.2f}s"
                )
            
            await asyncio.sleep(base + jitter)
            continue
    
    # Exhausted retries
    if hs.logger:
        hs.logger.error(
            f"{hs.bot_id} [{hs.domain}/{hs.profile}] "
            f"Failed after 5 attempts: {type(last_exc).__name__}"
        )
    raise last_exc


# ---------- Session Manager ----------
class CFSessionManager:
    """
    Manages HTTP sessions with Cloudflare challenge handling and rate limiting.
    Handles async/sync requests transparently.
    """
    
    def __init__(self, max_age=6 * 3600, logger=None, bot_id: str = "", verify: bool = True):
        self.max_age = max_age
        self._hosts: Dict[str, _HostState] = {}
        self._hosts_lock = asyncio.Lock()
        self.logger = logger
        self.bot_id = bot_id
        self.verify = verify
        
        # Bootstrap URLs for cookie minting
        self._bootstrap_urls: Dict[str, List[str]] = {
            "stripchat.com": [
                "https://stripchat.com/",
                "https://hu.stripchat.com/"
            ],
            "doppiocdn.com": ["https://stripchat.com/"],
            "doppiocdn.org": ["https://stripchat.com/"],
            "doppiocdn.net": ["https://stripchat.com/"],
        }

    async def _ensure_host(self, domain: str, bucket: str) -> _HostState:
        """Ensure host state exists with valid cookies."""
        key = f"{domain}|{bucket}"
        
        async with self._hosts_lock:
            if key not in self._hosts:
                visit = self._bootstrap_urls.get(domain, [f"https://{domain}/"])
                hs = _HostState(
                    domain, visit, profile=bucket,
                    logger=self.logger, bot_id=self.bot_id
                )
                
                # Load or mint cookies
                data = await load_or_mint(domain, visit, max_age=self.max_age)
                hs.apply_cookie_data(data)
                self._hosts[key] = hs
            
            return self._hosts[key]

    def _normalize_url(self, url_input: Union[str, tuple]) -> str:
        """Normalize URL input to string, handling tuples."""
        if isinstance(url_input, tuple):
            # Handle trailing comma case: ('url',)
            if len(url_input) == 1:
                url = url_input[0]
            else:
                # Multiple elements, take first
                url = url_input[0]
        else:
            url = url_input
        
        # Ensure string
        if not isinstance(url, str):
            raise TypeError(f"URL must be string, got {type(url).__name__}: {repr(url)}")
        
        return url.strip()

    async def _request_async(self, method: str, url_input: Union[str, tuple], **kwargs):
        """Perform async HTTP request with CF handling and rate limiting."""
        # Add verify parameter if not explicitly set
        if "verify" not in kwargs:
            kwargs["verify"] = self.verify
        
        # Ensure timeout is set (helps with HTTP/2 stream issues)
        if "timeout" not in kwargs:
            kwargs["timeout"] = 45.0  # Increased timeout to handle slower responses
            
        # Normalize URL
        try:
            url = self._normalize_url(url_input)
        except TypeError as e:
            if self.logger:
                self.logger.error(f"{self.bot_id} URL normalization failed: {e}")
            raise
        
        # Parse URL
        try:
            parsed = urlparse(url)
        except Exception as e:
            if self.logger:
                self.logger.error(f"{self.bot_id} URL parsing failed: {e}")
            raise
        
        domain = parsed.hostname or "default"
        bucket = kwargs.pop("bucket", None) or _infer_bucket(domain, parsed.path or "/")
        
        hs = await self._ensure_host(domain, bucket)
        
        if self.logger and parameters.DEBUG:
            self.logger.debug(f"{self.bot_id} [{domain}/{bucket}] {method} {url[:80]}")

        # Rate limiting with optional API serialization
        if bucket == "api" and hs.req_lock:
            async with hs.req_lock:
                await hs._acquire_slot()
                # Small random delay for API calls
                if random.random() < 0.25:
                    await asyncio.sleep(random.uniform(0.005, 0.02))
                r = await _perform_with_retries(hs, method, url, **kwargs)
        else:
            await hs._acquire_slot()
            r = await _perform_with_retries(hs, method, url, **kwargs)

        # Check for CF challenge
        if _is_cf_html_block(r):
            now = time.monotonic()
            if now - hs.last_mint_ts >= hs.mint_cooldown:
                async with hs.mint_lock:
                    # Double-check after acquiring lock
                    now2 = time.monotonic()
                    if now2 - hs.last_mint_ts >= hs.mint_cooldown:
                        if self.logger:
                            self.logger.info(
                                f"{self.bot_id} [{domain}/{bucket}] "
                                f"Cloudflare challenge detected, minting cookies..."
                            )
                        data = await mint_cookies_for(domain, hs.visit_urls)
                        write(domain, data)
                        hs.apply_cookie_data(data)
                        hs.last_mint_ts = now2
                
                # Brief pause then retry
                await asyncio.sleep(random.uniform(0.3, 0.7))
                r = await _perform_with_retries(hs, method, url, **kwargs)
            
            return r

        # Handle 429 rate limit
        if r.status_code == 429:
            await hs.note_429(r.headers.get("Retry-After"))
            
            # One retry after partial backoff
            await asyncio.sleep(min(2.0, hs.backoff / 2.0))
            await hs._acquire_slot()
            r2 = await _perform_with_retries(hs, method, url, **kwargs)
            
            if r2.status_code != 429:
                await hs.note_success()
            
            return r2

        await hs.note_success()
        return r

    # --- Sync API for Bot compatibility ---
    
    def request(self, method: str, url: Union[str, tuple], **kwargs):
        """Synchronous request wrapper."""
        url = self._normalize_url(url)
        return _run(self._request_async(method, url, **kwargs))

    def get(self, url: Union[str, tuple], **kwargs):
        """Synchronous GET request."""
        url = self._normalize_url(url)
        return self.request("GET", url, **kwargs)

    def post(self, url: Union[str, tuple], **kwargs):
        """Synchronous POST request."""
        url = self._normalize_url(url)
        return self.request("POST", url, **kwargs)

    # --- Async API ---
    
    async def arequest(self, method: str, url: Union[str, tuple], **kwargs):
        """Async request."""
        url = self._normalize_url(url)
        return await self._request_async(method, url, **kwargs)

    async def aget(self, url: Union[str, tuple], **kwargs):
        """Async GET request."""
        url = self._normalize_url(url)
        return await self._request_async("GET", url, **kwargs)

    async def apost(self, url: Union[str, tuple], **kwargs):
        """Async POST request."""
        url = self._normalize_url(url)
        return await self._request_async("POST", url, **kwargs)