from streamonitor.enums import Status


web_status_lookup = {
    Status.PUBLIC: "Online",
    Status.OFFLINE: "Offline",
    Status.PRIVATE: "Private Show",
    Status.NOTRUNNING: "Not Running",
    Status.RATELIMIT: "Rate-limited",
    Status.NOTEXIST: "No Such Streamer",
    Status.ERROR: "Error on Download",
    Status.UNKNOWN: "Unknown Error",
    Status.RESTRICTED: "Restricted: Geo-blocked?"
}

status_icons_lookup = {
    Status.UNKNOWN: "help-circle",
    Status.NOTRUNNING: "stop-circle",
    Status.ERROR: "alert-circle", 
    Status.RESTRICTED: "shield-x",
    Status.PUBLIC: "video",
    Status.NOTEXIST: "user-x",
    Status.PRIVATE: "lock",
    Status.OFFLINE: "video-off",
    Status.DELETED: "trash-2",
    Status.RATELIMIT: "clock",
    Status.CLOUDFLARE: "cloud"
}