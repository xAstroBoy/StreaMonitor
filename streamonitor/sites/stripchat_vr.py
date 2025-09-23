from parameters import VR_FORMAT_SUFFIX
from streamonitor.enums import Status
from streamonitor.sites.stripchat import StripChat
from streamonitor.bot import Bot


class StripChatVR(StripChat):
    site = 'StripChatVR'
    siteslug = 'SCVR'

    vr_frame_format_map = {
        'FISHEYE': 'F',
        'PANORAMIC': 'P',
        'CIRCULAR': 'C',
    }

    def __init__(self, username):
        super().__init__(username)
        self.stopDownloadFlag = False
        self.vr = True
        self.url = self.getWebsiteURL()

    @property
    def filename_extra_suffix(self):
        if not VR_FORMAT_SUFFIX:
            return ''

        bs = self.lastInfo.get('broadcastSettings') or {}
        vr_cam_settings = bs.get('vrCameraSettings')
        if isinstance(vr_cam_settings, dict):
            vr_packing = vr_cam_settings.get("stereoPacking", "M")
            vr_frame_format = self.vr_frame_format_map.get(vr_cam_settings.get("frameFormat"), "X")
            vr_angle = vr_cam_settings.get("horizontalAngle", 0)
            return f'_{vr_packing}_{vr_frame_format}{vr_angle}'
        return ''

    def getWebsiteURL(self):
        return "https://vr.stripchat.com/cam/" + self.username

    def getStatus(self):
        status = super(StripChatVR, self).getStatus()
        if status == Status.PUBLIC:
            # Check VR flag across APIs
            model = self.lastInfo.get("model", {})
            is_vr_model = isinstance(model, dict) and bool(model.get("isVr"))
            is_vr_direct = bool(self.lastInfo.get("isVr"))

            bs = self.lastInfo.get("broadcastSettings") or {}
            has_vr_settings = isinstance(bs.get("vrCameraSettings"), dict)

            if (is_vr_model or is_vr_direct) and has_vr_settings:
                return Status.PUBLIC
            return Status.OFFLINE
        return status

    def isMobile(self):
        return False


Bot.loaded_sites.add(StripChatVR)
