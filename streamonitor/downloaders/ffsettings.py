# streamonitor/downloaders/ffsettings.py
# FFmpeg downloader configuration settings

class FFSettings:
    """Configuration settings for FFmpeg HLS downloader."""
    
    # === HLS Streaming Settings ===
    LIVE_LAST_SEGMENTS = 3  # Start from last N segments (None to disable)
    RW_TIMEOUT_SEC = 5  # Read/write timeout
    SOCKET_TIMEOUT_SEC = 5  # Socket connection timeout
    RECONNECT_DELAY_MAX = 10  # Max delay between reconnection attempts
    MAX_RESTARTS_ON_STALL = 8  # Max times to restart ffmpeg on stall
    GRACEFUL_QUIT_TIMEOUT_SEC = 6  # Time to wait for graceful shutdown
    
    # === Startup & Stall Detection ===
    STARTUP_GRACE_SEC = 10  # Don't check for stalls during startup
    SUSPECT_STALL_SEC = 25  # No activity for this long triggers suspect mode
    CONFIRM_STALL_EXTRA_SEC = 10  # Extra time before confirming stall
    STALL_SAME_TIME_SEC = 20  # PTS frozen for this long = stall
    SPEED_LOW_THRESHOLD = 0.80  # Speed below this is "low"
    SPEED_LOW_SUSTAIN_SEC = 25  # Low speed sustained this long = stall
    MAX_SINGLE_LAG_SEC = 12  # Single lag event above this = immediate stall
    MAX_CONSEC_SKIP_LINES = 5  # Consecutive skip messages = stall
    FALLBACK_NO_STDERR_SEC = 35  # No stderr activity fallback
    FALLBACK_NO_OUTPUT_SEC = 35  # No output growth fallback
    
    # === Logging & Rate Limiting ===
    STALL_LOG_SUPPRESS_AFTER = 4  # Suppress individual stall logs after N
    AGG_LOG_INTERVAL_SEC = 120  # Aggregate log interval
    COOLDOWN_AFTER_CONSEC_STALLS = 3  # Cooldown after N consecutive stalls
    COOLDOWN_SLEEP_SEC = 60  # Cooldown duration
    STDERR_MIN_APPEND_BYTES = 24  # Min bytes for stderr activity
    LOOP_SLEEP_SEC = 0.15  # Main loop sleep interval
    DEBUG_TAIL_LINES = 120  # Lines to keep for debug output
    
    # === FFmpeg Return Codes ===
    RETCODE_OK = (0, 255)  # Acceptable return codes
    
    # === FFmpeg Probe/Analysis Settings ===
    PROBESIZE = "4M"  # Amount of data to probe
    ANALYSEDURATION_US = "10000000"  # 10 seconds in microseconds
    ENABLE_FFLAGS_NOBUFFER = True  # Disable buffering
    ENABLE_FFLAGS_DISCARDCORRUPT = True  # Discard corrupt packets
    USE_GENPTS = True  # Generate presentation timestamps
    USE_PROGRESS_FORCED_STATS = False  # Force progress output to stderr
    
    # === Playlist Probing ===
    PLAYLIST_PROBE_ENABLED = True  # Enable playlist change detection
    PLAYLIST_PROBE_INTERVAL_SEC = 8  # How often to check playlist
    PLAYLIST_STALE_THRESHOLD_SEC = 60  # Playlist unchanged for this = stale
    
    # === Output Format Settings ===
    # These map CONTAINER parameter to actual settings
    FORMAT_SETTINGS = {
        'ts': {
            'ext': '.ts',
            'format': 'mpegts',
            'segment_format': 'mpegts'
        },
        'mkv': {
            'ext': '.mkv',
            'format': 'matroska',
            'segment_format': 'matroska'
        },
        'mp4': {
            'ext': '.mp4',
            'format': 'mp4',
            'segment_format': 'mp4',
            'movflags': 'frag_keyframe+empty_moov',
            'segment_movflags': 'movflags=frag_keyframe+empty_moov'
        }
    }
    
    @classmethod
    def get_format_settings(cls, container):
        """
        Get format settings for a container type.
        
        Args:
            container: Container type ('ts', 'mkv', 'mp4')
            
        Returns:
            Dict of format settings
        """
        container = (container or "mkv").lower().strip()
        if container not in cls.FORMAT_SETTINGS:
            container = "mkv"  # Default to mkv for safety
        return cls.FORMAT_SETTINGS[container]
    
    @classmethod
    def validate_settings(cls):
        """
        Validate settings for common misconfigurations.
        Returns list of warnings.
        """
        warnings = []
        
        if cls.STARTUP_GRACE_SEC < 5:
            warnings.append("STARTUP_GRACE_SEC < 5 may cause false stall detections")
        
        if cls.MAX_RESTARTS_ON_STALL < 3:
            warnings.append("MAX_RESTARTS_ON_STALL < 3 may not give enough retry attempts")
        
        if cls.SUSPECT_STALL_SEC < cls.STARTUP_GRACE_SEC:
            warnings.append("SUSPECT_STALL_SEC should be >= STARTUP_GRACE_SEC")
        
        if cls.COOLDOWN_SLEEP_SEC < 30:
            warnings.append("COOLDOWN_SLEEP_SEC < 30 may not give stream enough time to recover")
        
        return warnings