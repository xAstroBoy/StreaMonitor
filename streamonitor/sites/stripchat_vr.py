from parameters import VR_FORMAT_SUFFIX
from streamonitor.downloaders.hls import getVideoNativeHLS
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

    # ────────────────────────────────
    # Internal VR helpers (uses parent's _first_in_paths / _recursive_find)
    # ────────────────────────────────
    def _find_vr_cam_settings(self):
        """
        Return vrCameraSettings dict if present in any known location, else None.
        """
        if not self.lastInfo:
            return None
        paths = [
            ["broadcastSettings", "vrCameraSettings"],
            ["cam", "broadcastSettings", "vrCameraSettings"],
            ["cam", "vrCameraSettings"],
            ["model", "broadcastSettings", "vrCameraSettings"],
            ["model", "vrCameraSettings"],
            ["user", "broadcastSettings", "vrCameraSettings"],
            ["user", "user", "broadcastSettings", "vrCameraSettings"],
            ["user", "user", "vrCameraSettings"],
            ["settings", "vrCameraSettings"],
        ]
        val = self._first_in_paths(paths)
        if isinstance(val, dict) and val:
            return val
        found = self._recursive_find(self.lastInfo, "vrCameraSettings")
        if isinstance(found, dict) and found:
            return found
        return None

    def _find_generic_settings(self):
        """
        Return generic settings dict if present in any known location, else None.
        """
        if not self.lastInfo:
            return None
        paths = [
            ["settings"],
            ["cam", "settings"],
            ["model", "settings"],
            ["user", "settings"],
            ["user", "user", "settings"],
            ["broadcastSettings"],
            ["cam", "broadcastSettings"],
            ["model", "broadcastSettings"],
            ["user", "user", "broadcastSettings"],
        ]
        val = self._first_in_paths(paths)
        if isinstance(val, dict) and val:
            return val
        found = self._recursive_find(self.lastInfo, "settings")
        if isinstance(found, dict) and found:
            return found
        # fallback: broadcastSettings may be dict but not under "settings"
        bs = self._first_in_paths([["broadcastSettings"], ["cam", "broadcastSettings"], ["model", "broadcastSettings"]])
        if isinstance(bs, dict) and bs:
            return bs
        return None

    # ────────────────────────────────
    # Public VR detectors (robust)
    # ────────────────────────────────
    def getIsVrModel(self) -> bool:
        """
        Detects explicit model-level isVr flags in common locations.
        """
        if not self.lastInfo:
            return False
        paths = [
            ["model", "isVr"],
            ["user", "user", "isVr"],
            ["user", "isVr"],
            ["cam", "isVr"],
            ["isVr"]
        ]
        val = self._first_in_paths(paths)
        if val is not None:
            return bool(val)
        found = self._recursive_find(self.lastInfo, "isVr")
        return bool(found) if found is not None else False

    def getIsVrDirect(self) -> bool:
        """
        Checks for an explicit root/direct isVr flag or top-level user flags.
        """
        if not self.lastInfo:
            return False
        paths = [
            ["isVr"],
            ["user", "user", "isVr"],
            ["user", "isVr"],
            ["model", "isVr"],
            ["cam", "isVr"]
        ]
        val = self._first_in_paths(paths)
        if val is not None:
            return bool(val)
        return False

    def getHasVrSettings(self) -> bool:
        """
        True if vrCameraSettings exist and contain expected VR keys.
        """
        vr_cs = self._find_vr_cam_settings()
        if isinstance(vr_cs, dict) and vr_cs:
            # presence of any typical VR key is enough
            if any(k in vr_cs for k in ("frameFormat", "stereoPacking", "horizontalAngle", "verticalAngle")):
                return True
            # accept non-empty dict as well
            return True
        # as a looser check, look for specific VR keys anywhere in payload
        for key in ("frameFormat", "stereoPacking", "horizontalAngle", "vrCameraSettings"):
            if self._recursive_find(self.lastInfo, key) is not None:
                return True
        return False

    def getHasGenericSettings(self) -> bool:
        """
        True if a generic 'settings' or broadcast settings object exists.
        """
        gs = self._find_generic_settings()
        return isinstance(gs, dict) and bool(gs)

    # ────────────────────────────────
    # Filename suffix
    # ────────────────────────────────
    @property
    def filename_extra_suffix(self):
        """
        Build a suffix from VR settings if available, otherwise try generic settings.
        Returns empty string when VR suffixing is disabled or no info found.
        """
        if not VR_FORMAT_SUFFIX:
            return ''

        # Prefer explicit vrCameraSettings
        vr_cam_settings = self._find_vr_cam_settings()
        if isinstance(vr_cam_settings, dict) and vr_cam_settings:
            vr_packing = vr_cam_settings.get("stereoPacking") or vr_cam_settings.get("packing") or "M"
            frame_format_raw = vr_cam_settings.get("frameFormat") or vr_cam_settings.get("frame_format") or ""
            vr_frame_format = self.vr_frame_format_map.get(frame_format_raw, frame_format_raw[:1].upper() if frame_format_raw else "X")
            vr_angle = vr_cam_settings.get("horizontalAngle") or vr_cam_settings.get("horizontal_angle") or vr_cam_settings.get("angle") or 0
            # normalize angle to int/str
            try:
                vr_angle_str = str(int(float(vr_angle)))
            except Exception:
                vr_angle_str = str(vr_angle)
            return f'_{vr_packing}_{vr_frame_format}{vr_angle_str}'

        # Fallback to generic settings (width/height/fps)
        settings = self._find_generic_settings()
        if isinstance(settings, dict) and settings:
            w = settings.get("width") or settings.get("w")
            h = settings.get("height") or settings.get("h")
            fps = settings.get("fps") or settings.get("frameRate") or settings.get("framerate")
            try:
                if w and h and fps:
                    return f'_GEN_{w}x{h}_{fps}fps'
            except Exception:
                pass

        return ''

    # ────────────────────────────────
    # URL / status / misc
    # ────────────────────────────────
    def getWebsiteURL(self):
        return "https://vr.stripchat.com/cam/" + self.username

    def getStatus(self):
        """
        For VR site we require the model to be VR-capable. If the base status is PUBLIC
        we return PUBLIC only when any VR indicator is present. Otherwise treat as OFFLINE.
        """
        status = super(StripChatVR, self).getStatus()

        if status == Status.PUBLIC:
            # If it's public but not VR, we don't want to treat it as SCVR public
            if self.getIsVrModel() and self.getIsVrDirect() and self.getHasVrSettings():
                return Status.PUBLIC
            return Status.OFFLINE

        return status

    def isMobile(self):
        # VR downloads are desktop only
        return False


Bot.loaded_sites.add(StripChatVR)
