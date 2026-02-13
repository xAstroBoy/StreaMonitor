import urllib.parse
from typing import Optional, Dict, Any, Tuple, List

import requests
from bs4 import BeautifulSoup
from streamonitor.bot import Bot
from streamonitor.enums import Status


class MyFreeCams(Bot):
    site: str = 'MyFreeCams'
    siteslug: str = 'MFC'

    def __init__(self, username: str) -> None:
        super().__init__(username)
        self.attrs: Dict[str, str] = {}
        self.videoUrl: Optional[str] = None
        self.url = self.getWebsiteURL()

    def get_site_color(self) -> Tuple[str, List[str]]:
        """Return the color scheme for this site."""
        return ("white", [])

    def getWebsiteURL(self) -> str:
        """Get the website URL for this streamer."""
        return f"https://www.myfreecams.com/#{self.username}"

    def getVideoUrl(self, refresh: bool = False) -> Optional[str]:
        """Get the video stream URL."""
        if not refresh:
            return self.videoUrl

        if 'data-cam-preview-model-id-value' not in self.attrs:
            return None

        try:
            sid = self.attrs.get('data-cam-preview-server-id-value', '')
            mid_str = self.attrs.get('data-cam-preview-model-id-value', '')
            
            if not sid or not mid_str:
                return None
                
            mid = 100000000 + int(mid_str)
            a = 'a_' if self.attrs.get('data-cam-preview-is-wzobs-value') == 'true' else ''
            
            playlist_url = f"https://previews.myfreecams.com/hls/NxServer/{sid}/ngrp:mfc_{a}{mid}.f4v_mobile_mhp1080_previewurl/playlist.m3u8"
            
            response = self.session.get(
                playlist_url,
                timeout=30,
                bucket='playlist'
            )
            
            if response.status_code != 200:
                return None
                
            return self.getWantedResolutionPlaylist(playlist_url)
            
        except (ValueError, KeyError) as e:
            self.logger.error(f"Error building video URL: {e}")
            return None
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error getting video URL: {e}")
            return None
        except Exception as e:
            self.logger.error(f"Unexpected error getting video URL: {e}")
            return None

    def getStatus(self) -> Status:
        """Check the current status of the stream."""
        try:
            response = self.session.get(
                f'https://share.myfreecams.com/{self.username}',
                timeout=30,
                bucket='status'
            )
            
            if response.status_code == 404:
                return Status.NOTEXIST
            elif response.status_code != 200:
                self.logger.warning(f"HTTP {response.status_code} for user {self.username}")
                return Status.UNKNOWN
                
            content = response.content
            
            # Look for tracking URL to verify model exists
            startpos = content.find(b'https://www.myfreecams.com/php/tracking.php?')
            if startpos == -1:
                return Status.NOTEXIST
                
            endpos = content.find(b'"', startpos)
            if endpos == -1:
                return Status.NOTEXIST
                
            url_bytes = content[startpos:endpos]
            try:
                url = urllib.parse.urlparse(url_bytes.decode('utf-8'))
                qs = urllib.parse.parse_qs(url.query)
                if 'model_id' not in qs:
                    return Status.NOTEXIST
            except Exception as e:
                self.logger.error(f"Error parsing tracking URL: {e}")
                return Status.NOTEXIST

            # Parse HTML for cam preview data
            try:
                doc = BeautifulSoup(content, 'html.parser')
                params = doc.find(class_='campreview')
                
                if params and params.attrs:
                    self.attrs = params.attrs
                    self.videoUrl = self.getVideoUrl(refresh=True)
                    
                    if self.videoUrl:
                        return Status.PUBLIC
                    else:
                        return Status.PRIVATE
                else:
                    return Status.OFFLINE
                    
            except Exception as e:
                self.logger.error(f"Error parsing HTML: {e}")
                return Status.ERROR
                
        except requests.exceptions.RequestException as e:
            self.logger.error(f"Network error checking status: {e}")
            return Status.ERROR
        except Exception as e:
            self.logger.error(f"Unexpected error: {e}")
            return Status.ERROR
    
    def isMobile(self) -> bool:
        """Check if this is a mobile broadcast."""
        return False


Bot.loaded_sites.add(MyFreeCams)
