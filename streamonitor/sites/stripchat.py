
import re
import time
import requests
import base64
import hashlib
import random

from streamonitor.bot import Bot
from streamonitor.downloaders.hls import getVideoNativeHLS
from streamonitor.enums import Status
from streamonitor.utils.CloudflareDetection import looks_like_cf_html

class StripChat(Bot):
    site = 'StripChat'
    siteslug = 'SC'
    isMobileBroadcast = False

    _static_data = None
    _main_js_data = None
    _doppio_js_data = None
    _mouflon_keys: dict = None
    _cached_keys: dict = None

    def __init__(self, username):
        if StripChat._static_data is None:
            StripChat._static_data = {}
            try:
                self.getInitialData()
            except Exception as e:
                StripChat._static_data = None
                raise e
        while StripChat._static_data == {}:
            time.sleep(1)
        super().__init__(username)
        self.vr = False
        self.url = self.getWebsiteURL()
        self.getVideo = lambda _, url, filename: getVideoNativeHLS(self, url, filename, StripChat.m3u_decoder)

    @classmethod
    def getInitialData(cls):
        r = requests.get('https://hu.stripchat.com/api/front/v3/config/static', headers=cls.headers)
        if r.status_code != 200:
            raise Exception("Failed to fetch static data from StripChat")
        StripChat._static_data = r.json().get('static')

        mmp_origin = StripChat._static_data['features']['MMPExternalSourceOrigin']
        mmp_version = StripChat._static_data['featuresV2']['playerModuleExternalLoading']['mmpVersion']
        mmp_base = f"{mmp_origin}/v{mmp_version}"

        r = requests.get(f"{mmp_base}/main.js", headers=cls.headers)
        if r.status_code != 200:
            raise Exception("Failed to fetch main.js from StripChat")
        StripChat._main_js_data = r.content.decode('utf-8')

        doppio_js_name = re.findall('require[(]"./(Doppio.*?[.]js)"[)]', StripChat._main_js_data)[0]

        r = requests.get(f"{mmp_base}/{doppio_js_name}", headers=cls.headers)
        if r.status_code != 200:
            raise Exception("Failed to fetch doppio.js from StripChat")
        StripChat._doppio_js_data = r.content.decode('utf-8')


    @classmethod
    def m3u_decoder(cls, content):

        def _decode(encrypted_b64: str, key: str) -> str:
            if cls._cached_keys is None:
                cls._cached_keys = {}
            if key not in cls._cached_keys:
                cls._cached_keys[key] = hashlib.sha256(key.encode("utf-8")).digest()
            key_bytes = cls._cached_keys[key]
            enc = base64.b64decode(encrypted_b64 + "==")
            out = bytearray()
            for i, b in enumerate(enc):
                out.append(b ^ key_bytes[i % len(key_bytes)])
            return out.decode("utf-8")

        # get pkey (returns (psch, pkey))
        _, pkey = StripChat._getMouflonFromM3U(content)
        if not pkey:
            return content

        key = cls.getMouflonDecKey(pkey)

        lines = content.splitlines()
        out = []
        i = 0
        tag = "#EXT-X-MOUFLON:FILE:"
        tlen = len(tag)

        while i < len(lines):
            line = lines[i]

            if line.startswith(tag):
                enc = line[tlen:]
                dec = None
                try:
                    dec = _decode(enc, key)  # decoded per-segment filename (e.g. abcd1234.mp4)
                except Exception:
                    pass

                # Replace the *next* line and SKIP it (do not keep media.mp4)
                if i + 1 < len(lines):
                    nxt = lines[i + 1]
                    if dec:
                        # replace media.mp4 or media.mp4?query
                        nxt = re.sub(r"media\.mp4(\?[^ \t\n\r]*)?", dec, nxt)
                    out.append(nxt)
                    i += 2
                    continue
                else:
                    # no next line? just drop the tag
                    i += 1
                    continue

            # drop any other mouflon metadata just in case
            if line.startswith("#EXT-X-MOUFLON"):
                i += 1
                continue

            out.append(line)
            i += 1

        return "\n".join(out)


    @classmethod
    def getMouflonDecKey(cls, pkey):
        if not cls._mouflon_keys:
            cls._mouflon_keys = {}

        if pkey in cls._mouflon_keys:
            return cls._mouflon_keys[pkey]

        key = re.findall(f'"{pkey}:(.*?)"', cls._doppio_js_data)[0]
        cls._mouflon_keys[pkey] = key
        return key

    @staticmethod
    def _getMouflonFromM3U(m3u8_doc):
        if '#EXT-X-MOUFLON:' in m3u8_doc:
            _mouflon_start = m3u8_doc.find('#EXT-X-MOUFLON:')
            if _mouflon_start > 0:
                _mouflon = m3u8_doc[_mouflon_start:m3u8_doc.find('\n', _mouflon_start)].strip().split(':')
                psch = _mouflon[2]
                pkey = _mouflon[3]
                return psch, pkey
        return None, None

    def getWebsiteURL(self):
        return "https://stripchat.com/" + self.username

    def getVideoUrl(self):
        return self.getWantedResolutionPlaylist(None)

    def getPlaylistVariants(self, url):
        stream_id = self.getStreamName()    
        url = "https://edge-hls.{host}/hls/{id}{vr}/master/{id}{vr}{auto}.m3u8".format(
                host='doppiocdn.com',
                id=stream_id,
                vr='_vr' if self.vr else '',
                auto='_auto' if not self.vr else ''
            )
        result = requests.get(url, headers=self.headers, cookies=self.cookies)
        m3u8_doc = result.content.decode("utf-8")
        psch, pkey = StripChat._getMouflonFromM3U(m3u8_doc)
        variants = super().getPlaylistVariants(m3u_data=m3u8_doc)
        return [variant | {'url': f'{variant["url"]}{"&" if "?" in variant["url"] else "?"}psch={psch}&pkey={pkey}'}
                for variant in variants]


    

    @staticmethod
    def uniq():
        chars = ''.join(chr(i) for i in range(ord('a'), ord('z')+1))
        chars += ''.join(chr(i) for i in range(ord('0'), ord('9')+1))
        return ''.join(random.choice(chars) for _ in range(16))

    def getStatus(self):
        def _as_dict(obj):
            return obj if isinstance(obj, dict) else {}

        url = f'https://stripchat.com/api/front/v1/broadcasts/{self.username}?uniq={StripChat.uniq()}'
        r = self.session.get(url, headers=self.headers)

        ct = (r.headers.get("content-type") or "").lower()
        body = r.text or ""

        if r.status_code == 404:
            return Status.NOTEXIST
        if r.status_code == 403:
            if looks_like_cf_html(body):
                self.logger.error(f'Cloudflare challenge (403) for {self.username}')
                return Status.CLOUDFLARE
            return Status.RESTRICTED
        if r.status_code == 429:
            self.logger.error(f'Rate limited (429) for {self.username}')
            return Status.RATELIMIT
        if r.status_code >= 500:
            if looks_like_cf_html(body):
                self.logger.error(f'Cloudflare challenge ({r.status_code}) for {self.username}')
                return Status.CLOUDFLARE
            self.logger.error(f'Server error {r.status_code} for {self.username}')
            return Status.UNKNOWN

        if "application/json" not in ct:
            snippet = body[:200].replace("\n", " ")
            self.logger.error(f'Non-JSON reply ({ct}) for {self.username}. Snippet: {snippet}')
            return Status.UNKNOWN
        if not body.strip():
            self.logger.error(f'Empty JSON body for {self.username} (status {r.status_code})')
            return Status.UNKNOWN

        try:
            raw = r.json()
        except Exception:
            snippet = body[:200].replace("\n", " ")
            self.logger.error(
                f'Failed to decode JSON for {self.username}. Status {r.status_code}. Snippet: {snippet}'
            )
            return Status.UNKNOWN

        self.lastInfo = _as_dict(raw.get("item"))
        info = self.lastInfo

        # fallback safe dicts
        settings = _as_dict(info.get("settings"))

        # isMobile flag
        self.isMobileBroadcast = info.get("isMobile") or settings.get("isMobile") or False

        # status
        status = info.get("status")

        if status == "public" and info.get("isLive") and not info.get("isBadStream"):
            return Status.PUBLIC
        if status in ["private", "groupShow", "p2p", "virtualPrivate", "p2pVoice"]:
            return Status.PRIVATE
        if not info.get("isLive") or status in ["off", "idle"]:
            return Status.OFFLINE

        self.logger.warning(
            f"Unknown status: {status} "
            f"isLive={info.get('isLive')} "
            f"isBadStream={info.get('isBadStream')}"
        )
        return Status.UNKNOWN

    def isMobile(self):
        return self.isMobileBroadcast

    def getStreamName(self) -> str:
        """
        Return the current streamName for this model.
        Handles both legacy API (cam.streamName) and new broadcasts API (streamName).
        Raises KeyError if not found.
        """
        if not self.lastInfo:
            raise KeyError("lastInfo is empty, call getStatus() first")

        # New API
        if "streamName" in self.lastInfo:
            return self.lastInfo["streamName"]

        # Old API fallback
        cam = self.lastInfo.get("cam")
        if isinstance(cam, dict) and "streamName" in cam:
            return cam["streamName"]

        raise KeyError(f"No streamName in lastInfo: keys={list(self.lastInfo.keys())}")


Bot.loaded_sites.add(StripChat)