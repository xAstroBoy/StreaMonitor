import json
import sys
import time

from streamonitor.bot import Bot
# Import all sites to register them with Bot.loaded_sites
import streamonitor.sites
import streamonitor.log as log

config_loc = "config.json"
logger = log.Logger("config")


def load_config():
    try:
        with open(config_loc, "r+") as f:
            return json.load(f)
    except FileNotFoundError:
        with open(config_loc, "w+") as f:
            json.dump([], f, indent=4)
            return []
    except Exception as e:
        logger.error(f"Failed to load config: {e}", exc_info=True)
        sys.exit(1)


def save_config(config):
    try:
        with open(config_loc, "w+") as f:
            json.dump(config, f, indent=4)

        return True
    except Exception as e:
        logger.error(f"Failed to save config: {e}", exc_info=True)
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
        # Restore gender and country from config
        gender_val = streamer.get('gender')
        if gender_val is not None:
            try:
                from streamonitor.enums import Gender
                streamer_bot.gender = Gender(gender_val)
            except (ValueError, KeyError):
                pass
        country = streamer.get('country')
        if country:
            streamer_bot.country = country
        streamers.append(streamer_bot)
        streamer_bot.start()
        if streamer["running"]:
            streamer_bot.restart()
            time.sleep(0.1)
    return streamers
