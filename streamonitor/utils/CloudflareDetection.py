class CloudflareDetection:
    @staticmethod
    def looks_like_cf_html(text: str) -> bool:
        if not text:
            return False
        t = text.lower()
        return (
            "<title>just a moment" in t
            or "/cdn-cgi/challenge-platform/" in t
            or "enable javascript and cookies to continue" in t
        )

# ⬇️ Add this shim so you can `from ... import looks_like_cf_html`
def looks_like_cf_html(text: str) -> bool:
    return CloudflareDetection.looks_like_cf_html(text)

__all__ = ["CloudflareDetection", "looks_like_cf_html"]
