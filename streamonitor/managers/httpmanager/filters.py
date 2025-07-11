from streamonitor.enums import Status
from streamonitor.managers.httpmanager.mappers import web_status_lookup, status_icons_lookup


def status_text(sc):
    if sc:
        return web_status_lookup.get(sc, web_status_lookup[Status.OFFLINE])
    else:
        return web_status_lookup.get(Status.UNKNOWN)


def status_icon(streamer):
    if streamer.recording:
        return 'arrow-down-circle'
    return status_icons_lookup.get(streamer.sc) or status_icons_lookup.get(Status.UNKNOWN)
