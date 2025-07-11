import requests
from streamonitor.bot import Bot
from streamonitor.enums import Status


class StripChat(Bot):
    site = 'StripChat'
    siteslug = 'SC'
    isMobileBroadcast = False

    def __init__(self, username):
        super().__init__(username)
        self.vr = False

    def getWebsiteURL(self):
        return "https://stripchat.com/" + self.username

    def getVideoUrl(self):
        return self.getWantedResolutionPlaylist(None)

    def getPlaylistVariants(self, url):
        def formatUrl(master, auto):
            return "https://edge-hls.{host}/hls/{id}{vr}/{master}/{id}{vr}{auto}.m3u8".format(
            host='doppiocdn.com',
            id=self.lastInfo["cam"]["streamName"],
            master='master' if master else '',
            auto='_auto' if auto else '',
            vr='_vr' if self.vr else '')

        variants = []
        variants.extend(super().getPlaylistVariants(formatUrl(True, False)))
        variants.extend(super().getPlaylistVariants(formatUrl(True, True)))
        variants.extend(super().getPlaylistVariants(formatUrl(False, True)))
        variants.extend(super().getPlaylistVariants(formatUrl(False, False)))
        return variants

    def getStatus(self):
        r = requests.get('https://stripchat.com/api/vr/v2/models/username/' + self.username, headers= self.headers, verify=False)
        if r.status_code != 200:
            return Status.UNKNOWN

        self.lastInfo = r.json()
        self.isMobileBroadcast = (
            self.lastInfo.get("broadcastSettings", {}).get("isMobile") or
            self.lastInfo.get("model", {}).get("isMobile") or
            False
        )
        if self.lastInfo["model"]["status"] == "public" and self.lastInfo["isCamAvailable"] and self.lastInfo['cam']["isCamActive"]:
            return Status.PUBLIC
        if self.lastInfo["model"]["status"] in ["private", "groupShow", "p2p", "virtualPrivate", "p2pVoice"]:
            return Status.PRIVATE
        if self.lastInfo["model"]["status"] in ["off", "idle"]:
            return Status.OFFLINE
        self.logger.warn(f'Got unknown status: {self.lastInfo["model"]["status"]} with isCamAvailable {self.lastInfo["isCamAvailable"] } and isCamActive { self.lastInfo["cam"]["isCamActive"] }')
        return Status.UNKNOWN

    def isMobile(self):
        return self.isMobileBroadcast

Bot.loaded_sites.add(StripChat)
