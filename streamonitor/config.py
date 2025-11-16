import json
import sys
import time

from parameters import CONFIG_FILE
from streamonitor.bot import Bot
# Import all sites to register them with Bot.loaded_sites
import streamonitor.sites


def load_config():
    try:
        with open(CONFIG_FILE, "r+") as f:
            return json.load(f)
    except FileNotFoundError:
        with open(CONFIG_FILE, "w+") as f:
            json.dump([], f, indent=4)
            return []
    except Exception as e:
        print(e)
        sys.exit(1)


def save_config(config):
    try:
        with open(CONFIG_FILE, "w+") as f:
            json.dump(config, f, indent=4)

        return True
    except Exception as e:
        print(e)
        sys.exit(1)


def loadStreamers():
    streamers = []
    for streamer in load_config():
        room_id = streamer.get('room_id')
        username = streamer["username"]
        site = streamer["site"]
        if room_id:
            streamer_bot = Bot.str2site(site)(username, room_id=room_id)
        else:
            streamer_bot = Bot.str2site(site)(username)
        streamers.append(streamer_bot)
        streamer_bot.start()
        if streamer["running"]:
            streamer_bot.restart()
            time.sleep(0.1)
    return streamers
