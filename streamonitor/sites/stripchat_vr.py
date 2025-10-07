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

        vr_cam_settings = self.lastInfo['broadcastSettings']['vrCameraSettings']
        if vr_cam_settings is not None:
            vr_packing = vr_cam_settings["stereoPacking"]
            vr_frame_format = self.vr_frame_format_map[vr_cam_settings["frameFormat"]]
            vr_angle = vr_cam_settings["horizontalAngle"]
            vr_suffix = f'_{vr_packing}_{vr_frame_format}{vr_angle}'
            return vr_suffix
        return ''

    def getWebsiteURL(self):
        return "https://vr.stripchat.com/cam/" + self.username

    def getStatus(self):
        status = super(StripChatVR, self).getStatus()
        if status == Status.PUBLIC:
            if self.lastInfo['model']['isVr'] and type(self.lastInfo['broadcastSettings']['vrCameraSettings']) is dict:
                return Status.PUBLIC
            return Status.OFFLINE
        return status
    def isMobile(self):
        return False

Bot.loaded_sites.add(StripChatVR)