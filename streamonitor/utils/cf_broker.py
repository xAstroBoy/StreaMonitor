# streamonitor/utils/cf_broker.py
import json, time
from pathlib import Path
from typing import Iterable
from playwright.async_api import async_playwright

COOKIES_DIR = Path("cookies")
COOKIES_DIR.mkdir(parents=True, exist_ok=True)

DEFAULT_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)

DEFAULT_HEADERS = {
    "User-Agent": DEFAULT_UA,
    "Accept": "*/*",
    "Accept-Language": "en-US,en;q=0.9",
    "Connection": "keep-alive",
}


def cookie_file_for(domain: str) -> Path:
    return COOKIES_DIR / f"{domain}.json"


def _serialize(cookies):
    keep = ("name", "value", "domain", "path", "expires", "secure", "httpOnly", "sameSite")
    return [{k: c.get(k) for k in keep} for c in cookies]


async def mint_cookies_for(domain: str, visit_urls: Iterable[str], timeout_ms=90000, settle_ms=3500, headless=True):
    async with async_playwright() as p:
        browser = await p.firefox.launch(headless=headless)
        ctx = await browser.new_context(
            user_agent=DEFAULT_UA,
            viewport={"width": 1280, "height": 800},
            locale="en-US",
        )
        page = await ctx.new_page()
        for url in visit_urls:
            await page.goto(url, wait_until="domcontentloaded", timeout=timeout_ms)
            await page.wait_for_timeout(settle_ms)
        cookies = await ctx.cookies()
        await ctx.close()
        await browser.close()

    data = {
        "ts": int(time.time()),
        "headers": dict(DEFAULT_HEADERS),
        "cookies": _serialize(cookies),
    }
    cookie_file_for(domain).write_text(json.dumps(data, indent=2))
    return data


async def load_or_mint(domain: str, visit_urls: Iterable[str], max_age=6 * 3600):
    cf = cookie_file_for(domain)
    if cf.exists():
        try:
            data = json.loads(cf.read_text())
            if int(time.time()) - int(data.get("ts", 0)) < max_age and data.get("cookies"):
                return data
        except Exception:
            pass
    return await mint_cookies_for(domain, visit_urls)


def write(domain: str, data: dict):
    data = dict(data)
    data["ts"] = int(time.time())
    cookie_file_for(domain).write_text(json.dumps(data, indent=2))
