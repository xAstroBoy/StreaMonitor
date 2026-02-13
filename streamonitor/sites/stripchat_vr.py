from parameters import VR_FORMAT_SUFFIX
from streamonitor.downloaders.hls import getVideoNativeHLS
from streamonitor.enums import Status
from streamonitor.sites.stripchat import StripChat


class StripChatVR(StripChat):
    site = 'StripChatVR'
    siteslug = 'SCVR'
    bulk_update = False  # VR doesn't support bulk status

    vr_frame_format_map = {
        'FISHEYE': 'F',
        'PANORAMIC': 'P',
        'CIRCULAR': 'C',
        'EQUIRECTANGULAR': 'E',
    }

    @property
    def filename_extra_suffix(self):
        """Add VR format suffix to filenames if configured."""
        if not VR_FORMAT_SUFFIX:
            return ''
        vr_settings = self._find_vr_cam_settings()
        if vr_settings:
            fmt = vr_settings.get('frameFormat', '')
            suffix = self.vr_frame_format_map.get(fmt, '')
            if suffix:
                return f'_{suffix}'
        return '_VR'

    def __init__(self, username, room_id=None):
        super().__init__(username, room_id=room_id)
        self.stopDownloadFlag = False
        self.vr = True
        self.url = self.getWebsiteURL()
        self._last_vr_capable_state = None  # Track VR capability state to avoid spam
    
    def get_site_color(self):
        """Return the color scheme for VR sites"""
        return ("cyan", ["bold"])

    def _find_vr_cam_settings(self):
        """
        Return vrCameraSettings dict if present in any known location, else None.
        """
        if not self.lastInfo:
            return None
        
        # Check common paths
        paths = [
            ["vrCameraSettings"],
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
        
        # Recursive search as fallback
        found = self._recursive_find(self.lastInfo, "vrCameraSettings")
        if isinstance(found, dict) and found:
            return found
        
        return None

    def _find_broadcast_settings(self):
        """
        Return broadcast settings dict if present in any known location, else None.
        """
        if not self.lastInfo:
            return None
        
        paths = [
            ["broadcastSettings"],
            ["cam", "broadcastSettings"],
            ["model", "broadcastSettings"],
            ["user", "broadcastSettings"],
            ["user", "user", "broadcastSettings"],
        ]
        
        val = self._first_in_paths(paths)
        if isinstance(val, dict) and val:
            return val
        
        found = self._recursive_find(self.lastInfo, "broadcastSettings")
        if isinstance(found, dict) and found:
            return found
        
        return None

    def _find_generic_settings(self):
        """
        Return generic settings dict if present in any known location, else None.
        Tries both 'settings' and 'broadcastSettings'.
        """
        if not self.lastInfo:
            return None
        
        paths = [
            ["settings"],
            ["cam", "settings"],
            ["model", "settings"],
            ["user", "settings"],
            ["user", "user", "settings"],
        ]
        
        val = self._first_in_paths(paths)
        if isinstance(val, dict) and val:
            return val
        
        # Try broadcastSettings as fallback
        bs = self._find_broadcast_settings()
        if bs:
            return bs
        
        # Recursive search
        found = self._recursive_find(self.lastInfo, "settings")
        if isinstance(found, dict) and found:
            return found
        
        return None

    def isVrCapable(self) -> bool:
        """
        Comprehensive VR capability check. Returns True if ANY VR indicator is found.
        This is more flexible than requiring ALL indicators to be present.
        """
        if not self.lastInfo:
            return False

        # Check 1: Explicit isVr flags - MOST IMPORTANT CHECK
        paths = [
            ["isVr"],
            ["model", "isVr"],
            ["user", "user", "isVr"],
            ["user", "isVr"],
            ["cam", "isVr"],
        ]
        val = self._first_in_paths(paths)
        if val is not None:
            # If isVr is explicitly False, model is NOT VR-capable
            if val is False:
                return False
            # If isVr is explicitly True, model IS VR-capable
            if val is True:
                return True

        # Check 2: VR camera settings exist AND are not empty
        vr_cam_settings = self._find_vr_cam_settings()
        if isinstance(vr_cam_settings, dict) and vr_cam_settings:
            # For VR settings to be valid, they must be a non-empty dict/list with actual VR data
            if isinstance(vr_cam_settings, list) and len(vr_cam_settings) == 0:
                return False  # Empty array means no VR settings
            
            # Valid VR settings should have at least one of these keys
            vr_keys = ("frameFormat", "stereoPacking", "horizontalAngle", "verticalAngle", 
                      "frame_format", "stereo_packing", "horizontal_angle", "vertical_angle",
                      "packing", "angle")
            if any(k in vr_cam_settings for k in vr_keys):
                return True

        # Check 3: Broadcast settings indicate VR (but be careful of default configs)
        broadcast_settings = self._find_broadcast_settings()
        if isinstance(broadcast_settings, dict) and broadcast_settings:
            # Check for VR-specific fields in broadcast settings
            vr_settings = broadcast_settings.get("vrCameraSettings")
            if vr_settings:
                # Must be non-empty and contain actual VR data
                if isinstance(vr_settings, list) and len(vr_settings) > 0:
                    return True
                elif isinstance(vr_settings, dict) and vr_settings:
                    # Check if it has actual VR data, not just default config
                    vr_keys = ("frameFormat", "stereoPacking", "horizontalAngle", "verticalAngle")
                    if any(k in vr_settings for k in vr_keys):
                        return True
            
            if broadcast_settings.get("isVr") is True:
                return True

        # If we reach here, no VR indicators found
        return False

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

    def getHasVrSettings(self) -> bool:
        """
        True if vrCameraSettings exist and contain expected VR keys.
        """
        vr_cs = self._find_vr_cam_settings()
        if isinstance(vr_cs, dict) and vr_cs:
            # Presence of any typical VR key is enough
            vr_keys = ("frameFormat", "stereoPacking", "horizontalAngle", "verticalAngle",
                      "frame_format", "stereo_packing", "horizontal_angle", "vertical_angle",
                      "packing", "angle")
            if any(k in vr_cs for k in vr_keys):
                return True
            # Accept non-empty dict as well (might have VR data in unexpected format)
            return len(vr_cs) > 0
        
        # Looser check: look for specific VR keys anywhere in payload
        for key in ("frameFormat", "stereoPacking", "horizontalAngle", "vrCameraSettings"):
            if self._recursive_find(self.lastInfo, key) is not None:
                return True
        
        return False

    @property
    def filename_extra_suffix(self):
        """
        Build a suffix from VR settings if available, otherwise try generic settings.
        Returns empty string when VR suffixing is disabled or no info found.
        """
        if not VR_FORMAT_SUFFIX:
            return ''

        try:
            # Prefer explicit vrCameraSettings
            vr_cam_settings = self._find_vr_cam_settings()
            if isinstance(vr_cam_settings, dict) and vr_cam_settings:
                # Get packing (stereo format)
                vr_packing = (vr_cam_settings.get("stereoPacking") or 
                            vr_cam_settings.get("stereo_packing") or 
                            vr_cam_settings.get("packing") or "M")
                
                # Get frame format
                frame_format_raw = (vr_cam_settings.get("frameFormat") or 
                                  vr_cam_settings.get("frame_format") or "")
                vr_frame_format = self.vr_frame_format_map.get(
                    frame_format_raw, 
                    frame_format_raw[:1].upper() if frame_format_raw else "X"
                )
                
                # Get angle
                vr_angle = (vr_cam_settings.get("horizontalAngle") or 
                          vr_cam_settings.get("horizontal_angle") or 
                          vr_cam_settings.get("angle") or 0)
                
                # Normalize angle to int/str
                try:
                    vr_angle_str = str(int(float(vr_angle)))
                except (ValueError, TypeError):
                    vr_angle_str = str(vr_angle) if vr_angle else "0"
                
                return f'_{vr_packing}_{vr_frame_format}{vr_angle_str}'

            # Fallback to generic settings (width/height/fps)
            settings = self._find_generic_settings()
            if isinstance(settings, dict) and settings:
                w = settings.get("width") or settings.get("w")
                h = settings.get("height") or settings.get("h")
                fps = (settings.get("fps") or 
                      settings.get("frameRate") or 
                      settings.get("framerate") or 
                      settings.get("frame_rate"))
                
                if w and h and fps:
                    try:
                        return f'_GEN_{int(w)}x{int(h)}_{int(fps)}fps'
                    except (ValueError, TypeError):
                        pass
        
        except Exception as e:
            # Log but don't crash
            self.logger.warning(f'Error building VR filename suffix: {e}')
        
        return ''

    def getWebsiteURL(self):
        return f"https://vr.stripchat.com/cam/{self.username}"

    def getStatus(self):
        """
        For VR site we require the model to be VR-capable. If the base status is PUBLIC
        we return PUBLIC only when VR indicators are present. Otherwise treat as OFFLINE.
        
        Uses flexible OR logic: if ANY VR indicator is found, consider it VR-capable.
        """
        status = super(StripChatVR, self).getStatus()

        if status == Status.PUBLIC:
            # Use comprehensive VR check - returns True if ANY VR indicator found
            vr_capable = self.isVrCapable()
            
            if vr_capable:
                # VR-capable, update state and return PUBLIC
                if self._last_vr_capable_state is False:
                    self.logger.info(f'{self.username} VR capability restored')
                self._last_vr_capable_state = True
                return Status.PUBLIC
            
            # Not VR-capable - only log if state changed
            if self._last_vr_capable_state is not False:
                self.logger.info(f'{self.username} is public but not VR-capable - going offline')
                self._last_vr_capable_state = False
            
            return Status.OFFLINE

        # For all other statuses (PRIVATE, OFFLINE, etc.), return as-is
        return status

    def isMobile(self):
        """VR downloads are desktop only."""
        return False
