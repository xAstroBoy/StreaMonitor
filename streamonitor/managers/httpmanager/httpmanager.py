# streamonitor/managers/httpmanager.py
# Improved Flask web interface with type hints and better error handling

from itertools import islice
from typing import cast, Union, Optional, Tuple, Dict, Any, List
from functools import wraps

from flask import Flask, make_response, render_template, request, send_from_directory, Response
import os
import json
import logging

from parameters import WEBSERVER_HOST, WEBSERVER_PORT, WEBSERVER_PASSWORD, WEB_LIST_FREQUENCY, WEB_STATUS_FREQUENCY
import streamonitor.log as log
from secrets import compare_digest
from streamonitor.bot import Bot
from streamonitor.enums import Status
from streamonitor.manager import Manager
from streamonitor.managers.outofspace_detector import OOSDetector
from streamonitor.utils import human_file_size

from .filters import status_icon, status_text
from .mappers import web_status_lookup
from .models import InvalidStreamer
from .utils import confirm_deletes, streamer_list, get_recording_query_params, get_streamer_context, set_streamer_list_cookies


class HTTPManager(Manager):
    """Flask-based web interface for managing stream recordings."""
    
    def __init__(self, streamers: List[Bot]) -> None:
        """
        Initialize HTTP manager with list of streamer bots.
        
        Args:
            streamers: List of Bot instances to manage
        """
        super().__init__(streamers)
        self.logger = log.Logger("manager")
        self.loaded_site_names: List[str] = [site.site for site in Bot.loaded_sites]
        self.loaded_site_names.sort()
        self.app: Optional[Flask] = None

    def _check_auth(self, username: str, password: str) -> bool:
        """
        Verify basic auth credentials.
        
        Args:
            username: Provided username (must be 'admin')
            password: Provided password
            
        Returns:
            True if auth is disabled or credentials match
        """
        return WEBSERVER_PASSWORD == "" or (
            username == 'admin' and compare_digest(password, WEBSERVER_PASSWORD)
        )

    def _login_required(self, f):
        """
        Decorator to require authentication on routes.
        
        Args:
            f: Flask route function to wrap
            
        Returns:
            Wrapped function that checks auth
        """
        @wraps(f)
        def wrapped_view(**kwargs):
            auth = request.authorization
            if WEBSERVER_PASSWORD != "" and not (
                auth and self._check_auth(auth.username, auth.password)
            ):
                return ('Unauthorized', 401, {
                    'WWW-Authenticate': 'Basic realm="Login Required"'
                })
            return f(**kwargs)
        return wrapped_view

    def _get_streamer_safe(self, user: str, site: str) -> Tuple[Optional[Bot], bool, str]:
        """
        Safely get streamer with error handling.
        
        Args:
            user: Username
            site: Site slug
            
        Returns:
            Tuple of (streamer, has_error, error_message)
        """
        try:
            streamer = self.getStreamer(user, site)
            if streamer is None:
                return None, True, f"Streamer {user} on {site} not found"
            return streamer, False, ""
        except Exception as e:
            self.logger.error(f"Error getting streamer {user}/{site}: {e}")
            return None, True, str(e)

    def _make_json_response(self, data: Dict[str, Any], status: int = 200) -> Response:
        """
        Create JSON response with proper headers.
        
        Args:
            data: Dict to serialize to JSON
            status: HTTP status code
            
        Returns:
            Flask Response object
        """
        return Response(
            json.dumps(data),
            status=status,
            mimetype='application/json'
        )

    def run(self) -> None:
        """Start the Flask web server."""
        app = Flask(__name__, "")
        self.app = app
        
        # Disable werkzeug logging (too verbose)
        werkzeug_logger = logging.getLogger('werkzeug')
        werkzeug_logger.disabled = True

        # Register template filters
        app.add_template_filter(human_file_size, name='tohumanfilesize')
        app.add_template_filter(status_icon, name='status_icon_class')
        app.add_template_filter(status_text, name='status_text')

        # ====================================================================
        # STATIC & DASHBOARD ROUTES
        # ====================================================================

        @app.route('/dashboard')
        @self._login_required
        def mainSite() -> Response:
            """Main dashboard page."""
            return app.send_static_file('index.html')

        # ====================================================================
        # API ROUTES
        # ====================================================================

        @app.route('/api/basesettings')
        @self._login_required
        def apiBaseSettings() -> Response:
            """Get site and status configuration."""
            json_sites: Dict[str, str] = {}
            for site in Bot.loaded_sites:
                json_sites[site.siteslug] = site.site

            json_status: Dict[int, str] = {}
            for status in Status:
                json_status[status.value] = Bot.status_messages[status]

            return self._make_json_response({
                "sites": json_sites,
                "status": json_status,
            })

        @app.route('/api/data')
        @self._login_required
        def apiData() -> Response:
            """Get current streamer data and disk space."""
            json_streamer: List[Dict[str, Any]] = []
            
            for streamer in self.streamers:
                json_stream = {
                    "site": streamer.siteslug,
                    "running": streamer.running,
                    "recording": streamer.recording,
                    "sc": streamer.sc.value,
                    "status": streamer.status(),
                    "url": streamer.getWebsiteURL(),
                    "username": streamer.username
                }
                json_streamer.append(json_stream)

            try:
                free_space_pct = OOSDetector.free_space()
                space_usage = OOSDetector.space_usage()
            except Exception as e:
                self.logger.error(f"Error getting disk space: {e}")
                free_space_pct = 0.0
                space_usage = type('obj', (object,), {'free': 0})()

            return self._make_json_response({
                "streamers": json_streamer,
                "freeSpace": {
                    "percentage": str(round(free_space_pct, 3)),
                    "absolute": human_file_size(space_usage.free)
                }
            })

        @app.route('/api/command')
        @self._login_required
        def execApiCommand() -> str:
            """Execute management command via API."""
            command = request.args.get("command")
            if not command:
                return "Missing command parameter", 400
            
            try:
                return self.execCmd(command)
            except Exception as e:
                self.logger.error(f"Command execution error: {e}")
                return str(e), 500

        # ====================================================================
        # MAIN LIST VIEW ROUTES
        # ====================================================================

        @app.route('/', methods=['GET'])
        @self._login_required
        def status() -> str:
            """Main streamer list page."""
            try:
                usage = OOSDetector.space_usage()
                streamers, filter_context = streamer_list(self.streamers, request)
                
                context = {
                    'streamers': streamers,
                    'sites': self.loaded_site_names,
                    'unique_sites': set(map(lambda x: x.site, self.streamers)),
                    'streamer_statuses': web_status_lookup,
                    'free_space': human_file_size(usage.free),
                    'total_space': human_file_size(usage.total),
                    'percentage_free': round(usage.free / usage.total * 100, 3),
                    'refresh_freq': WEB_LIST_FREQUENCY,
                    'confirm_deletes': confirm_deletes(request.headers.get('User-Agent')),
                } | filter_context
                
                return render_template('index.html.jinja', **context)
            except Exception as e:
                self.logger.error(f"Error rendering status page: {e}")
                return f"Internal server error: {e}", 500

        @app.route('/refresh/streamers', methods=['GET'])
        @self._login_required
        def refresh_streamers() -> Response:
            """Refresh streamer list (HTMX endpoint)."""
            try:
                streamers, filter_context = streamer_list(self.streamers, request)
                
                context = {
                    'streamers': streamers,
                    'sites': Bot.loaded_sites,
                    'refresh_freq': WEB_LIST_FREQUENCY,
                    'toast_status': "hide",
                    'toast_message': "",
                    'confirm_deletes': confirm_deletes(request.headers.get('User-Agent')),
                } | filter_context
                
                response = make_response(render_template('streamers_result.html.jinja', **context))
                set_streamer_list_cookies(filter_context, request, response)
                return response
            except Exception as e:
                self.logger.error(f"Error refreshing streamers: {e}")
                return f"Error: {e}", 500

        # ====================================================================
        # RECORDING/VIDEO ROUTES
        # ====================================================================

        @app.route('/recordings/<user>/<site>', methods=['GET'])
        @self._login_required
        def recordings(user: str, site: str) -> Tuple[str, int]:
            """Recording list page for specific streamer."""
            video = request.args.get("play_video")
            sort_by_size = bool(request.args.get("sorted", False))
            
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            if has_error or streamer is None:
                return f"Error: {error_msg}", 500

            try:
                streamer.cache_file_list()
                context = get_streamer_context(
                    streamer, sort_by_size, video, 
                    request.headers.get('User-Agent')
                )
                
                status_code = 500 if context['has_error'] else 200
                
                # Auto-select video to play
                if video is None and streamer.recording and len(context['videos']) > 1:
                    video_index = 0 if sort_by_size else 1
                    context['video_to_play'] = next(
                        islice(context['videos'].values(), video_index, video_index + 1)
                    )
                elif video is None and len(context['videos']) > 0 and not streamer.recording:
                    context['video_to_play'] = next(islice(context['videos'].values(), 0, 1))
                
                return render_template('recordings.html.jinja', **context), status_code
            except Exception as e:
                self.logger.error(f"Error loading recordings for {user}/{site}: {e}")
                return f"Internal error: {e}", 500

        @app.route('/video/<user>/<site>/<path:filename>', methods=['GET'])
        def get_video(user: str, site: str, filename: str) -> Response:
            """Serve video file (no auth for video playback)."""
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            if has_error or streamer is None:
                return f"Error: {error_msg}", 404

            try:
                return send_from_directory(
                    os.path.abspath(streamer.outputFolder),
                    filename
                )
            except FileNotFoundError:
                return "Video not found", 404
            except Exception as e:
                self.logger.error(f"Error serving video {filename}: {e}")
                return f"Error: {e}", 500

        @app.route('/videos/watch/<user>/<site>/<path:play_video>', methods=['GET'])
        @self._login_required
        def watch_video(user: str, site: str, play_video: str) -> Tuple[Response, int]:
            """Watch specific video (HTMX endpoint)."""
            sort_by_size = bool(request.args.get("sorted", False))
            
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            if has_error or streamer is None:
                return make_response(f"Error: {error_msg}"), 500

            try:
                context = get_streamer_context(
                    streamer, sort_by_size, play_video,
                    request.headers.get('User-Agent')
                )
                
                status_code = 500 if context['video_to_play'] is None or context['has_error'] else 200
                response = make_response(
                    render_template('recordings_content.html.jinja', **context),
                    status_code
                )
                
                query_param = get_recording_query_params(sort_by_size, play_video)
                response.headers['HX-Replace-Url'] = f"/recordings/{user}/{site}{query_param}"
                return response, status_code
            except Exception as e:
                self.logger.error(f"Error watching video {play_video}: {e}")
                return make_response(f"Error: {e}"), 500

        @app.route('/videos/<user>/<site>', methods=['GET'])
        @self._login_required
        def sort_videos(user: str, site: str) -> Tuple[Response, int]:
            """Sort video list (HTMX endpoint)."""
            sort_by_size = bool(request.args.get("sorted", False))
            play_video = request.args.get("play_video", None)
            
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            if has_error or streamer is None:
                return make_response(f"Error: {error_msg}"), 500

            try:
                context = get_streamer_context(
                    streamer, sort_by_size, play_video,
                    request.headers.get('User-Agent')
                )
                
                status_code = 500 if context['has_error'] else 200
                response = make_response(
                    render_template('video_list.html.jinja', **context),
                    status_code
                )
                
                query_param = get_recording_query_params(sort_by_size, play_video)
                response.headers['HX-Replace-Url'] = f"/recordings/{user}/{site}{query_param}"
                return response, status_code
            except Exception as e:
                self.logger.error(f"Error sorting videos: {e}")
                return make_response(f"Error: {e}"), 500

        @app.route('/videos/<user>/<site>/<path:filename>', methods=['DELETE'])
        @self._login_required
        def delete_video(user: str, site: str, filename: str) -> Tuple[Response, int]:
            """Delete video file."""
            sort_by_size = bool(request.args.get("sorted", False))
            play_video = request.args.get("play_video", None)
            
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            if has_error or streamer is None:
                return make_response(f"Error: {error_msg}"), 500

            try:
                context = get_streamer_context(
                    streamer, sort_by_size, play_video,
                    request.headers.get('User-Agent')
                )
                
                status_code = 200
                match = context['videos'].pop(filename, None)
                
                if match is not None:
                    try:
                        os.remove(match.abs_path)
                        streamer.cache_file_list()
                        context['total_size'] = context['total_size'] - match.filesize
                        
                        if context['video_to_play'] is not None and \
                           filename == context['video_to_play'].filename:
                            context['video_to_play'] = None
                    except OSError as e:
                        status_code = 500
                        context['has_error'] = True
                        context['recordings_error_message'] = f"Failed to delete file: {e}"
                        self.logger.error(f"Failed to delete {filename}: {e}")
                else:
                    status_code = 404
                    context['has_error'] = True
                    context['recordings_error_message'] = f'Could not find {filename}'
                
                response = make_response(
                    render_template('video_list.html.jinja', **context),
                    status_code
                )
                
                query_param = get_recording_query_params(sort_by_size, play_video)
                response.headers['HX-Replace-Url'] = f"/recordings/{user}/{site}{query_param}"
                return response, status_code
            except Exception as e:
                self.logger.error(f"Error deleting video {filename}: {e}")
                return make_response(f"Error: {e}"), 500

        # ====================================================================
        # STREAMER MANAGEMENT ROUTES
        # ====================================================================

        @app.route("/add", methods=['POST'])
        @self._login_required
        def add() -> Tuple[str, int]:
            """Add new streamer."""
            user = request.form.get("username", "").strip()
            site = request.form.get("site", "").strip()
            
            if not user or not site:
                return "Missing username or site", 400

            try:
                update_site_options = site not in map(lambda x: x.site, self.streamers)
                toast_status = "success"
                status_code = 200
                
                streamer = self.getStreamer(user, site)
                res = self.do_add(streamer, user, site)
                
                streamers, filter_context = streamer_list(self.streamers, request)
                
                if res in ('Streamer already exists', "Missing value(s)", "Failed to add"):
                    toast_status = "error"
                    status_code = 500
                
                context = {
                    'streamers': streamers,
                    'unique_sites': set(map(lambda x: x.site, self.streamers)),
                    'update_filter_site_options': update_site_options,
                    'refresh_freq': WEB_LIST_FREQUENCY,
                    'toast_status': toast_status,
                    'toast_message': res,
                    'confirm_deletes': confirm_deletes(request.headers.get('User-Agent')),
                } | filter_context
                
                return render_template('streamers_result.html.jinja', **context), status_code
            except Exception as e:
                self.logger.error(f"Error adding streamer {user}/{site}: {e}")
                return f"Error: {e}", 500

        @app.route("/remove/<user>/<site>", methods=['DELETE'])
        @self._login_required
        def remove_streamer(user: str, site: str) -> Tuple[Union[str, Response], int]:
            """Remove streamer."""
            try:
                streamer = self.getStreamer(user, site)
                res = self.do_remove(streamer, user, site)
                
                if res in ("Failed to remove streamer", "Streamer not found"):
                    status_code = 404
                    context = {'streamer_error_message': res}
                    response = make_response(
                        render_template('streamer_record_error.html.jinja', **context),
                        status_code
                    )
                    response.headers['HX-Retarget'] = "#error-container"
                    return response, status_code
                
                return '', 204
            except Exception as e:
                self.logger.error(f"Error removing streamer {user}/{site}: {e}")
                return f"Error: {e}", 500

        @app.route("/toggle/<user>/<site>", methods=['PATCH'])
        @self._login_required
        def toggle_streamer(user: str, site: str) -> Tuple[str, int]:
            """Toggle streamer running state."""
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            
            if has_error or streamer is None:
                status_code = 500
                res = error_msg
            elif streamer.running:
                res = self.do_stop(streamer, user, site)
                has_error = res != "OK"
                status_code = 200 if not has_error else 500
            else:
                res = self.do_start(streamer, user, site)
                has_error = res != "OK"
                status_code = 200 if not has_error else 500
            
            context = {
                'streamer': streamer or InvalidStreamer(user, site),
                'streamer_has_error': has_error,
                'streamer_error_message': res if has_error else "",
                'confirm_deletes': confirm_deletes(request.headers.get('User-Agent')),
            }
            
            return render_template('streamer_record.html.jinja', **context), status_code

        @app.route("/toggle/<user>/<site>/recording", methods=['PATCH'])
        @self._login_required
        def toggle_streamer_recording_page(user: str, site: str) -> Tuple[str, int]:
            """Toggle streamer from recording page."""
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            
            if has_error or streamer is None:
                status_code = 500
                res = error_msg
            elif streamer.running:
                res = self.do_stop(streamer, user, site)
                has_error = res != "OK"
                status_code = 200 if not has_error else 500
            else:
                res = self.do_start(streamer, user, site)
                has_error = res != "OK"
                status_code = 200 if not has_error else 500
            
            context = {
                'streamer': streamer or InvalidStreamer(user, site),
                'streamer_has_error': has_error,
                'streamer_error_message': res if has_error else "",
            }
            
            return render_template('streamer_toggle.html.jinja', **context), status_code

        @app.route("/start/streamers", methods=['PATCH'])
        @self._login_required
        def start_streamers() -> Tuple[str, int]:
            """Start all or filtered streamers."""
            try:
                streamers, filter_context = streamer_list(self.streamers, request)
                status_code = 500
                toast_status = "error"
                res = ""
                error_message = ""
                
                if not filter_context.get('filtered') or len(streamers) == len(self.streamers):
                    res = self.do_start(None, '*', None)
                    if res == "Started all":
                        status_code = 200
                        toast_status = "success"
                else:
                    errors: List[str] = []
                    if len(streamers) > 0:
                        for streamer in streamers:
                            partial_res = self.do_start(streamer, None, None)
                            if partial_res != "OK":
                                errors.append(streamer.username)
                        res = "Started All Shown"
                    else:
                        res = 'no matching streamers'
                    
                    if len(errors) > 0:
                        toast_status = "warning"
                        res = "Some Failed to Start"
                        error_message = "Failed to start:\n" + '\n'.join(errors)
                    else:
                        status_code = 200
                        toast_status = "success"
                
                context = {
                    'streamers': streamers,
                    'refresh_freq': WEB_LIST_FREQUENCY,
                    'toast_status': toast_status,
                    'toast_message': res,
                    'error_message': error_message,
                    'confirm_deletes': confirm_deletes(request.headers.get('User-Agent')),
                } | filter_context
                
                return render_template('streamers_result.html.jinja', **context), status_code
            except Exception as e:
                self.logger.error(f"Error starting streamers: {e}")
                return f"Error: {e}", 500

        @app.route("/stop/streamers", methods=['PATCH'])
        @self._login_required
        def stop_streamers() -> Tuple[str, int]:
            """Stop all or filtered streamers."""
            try:
                streamers, filter_context = streamer_list(self.streamers, request)
                status_code = 500
                toast_status = "error"
                res = ""
                error_message = ""
                
                if not filter_context.get('filtered') or len(streamers) == len(self.streamers):
                    res = self.do_stop(None, '*', None)
                    if res == "Stopped all":
                        status_code = 200
                        toast_status = "success"
                else:
                    errors: List[str] = []
                    if len(streamers) > 0:
                        for streamer in streamers:
                            partial_res = self.do_stop(streamer, None, None)
                            if partial_res != "OK":
                                errors.append(streamer.username)
                        res = "Stopped All Shown"
                    else:
                        res = 'no matching streamers'
                    
                    if len(errors) > 0:
                        toast_status = "warning"
                        res = "Some Failed to Stop"
                        error_message = "Failed to stop:\n" + '\n'.join(errors)
                    else:
                        status_code = 200
                        toast_status = "success"
                
                context = {
                    'streamers': streamers,
                    'refresh_freq': WEB_LIST_FREQUENCY,
                    'toast_status': toast_status,
                    'toast_message': res,
                    'error_message': error_message,
                    'confirm_deletes': confirm_deletes(request.headers.get('User-Agent')),
                } | filter_context
                
                return render_template('streamers_result.html.jinja', **context), status_code
            except Exception as e:
                self.logger.error(f"Error stopping streamers: {e}")
                return f"Error: {e}", 500

        # ====================================================================
        # UTILITY ROUTES
        # ====================================================================

        @app.route("/clear", methods=['DELETE'])
        def clear_modal() -> Tuple[str, int]:
            """Clear modal (HTMX utility)."""
            return '', 204

        @app.route("/recording/nav/<user>/<site>", methods=['GET'])
        @self._login_required
        def get_streamer_navbar(user: str, site: str) -> Tuple[str, int]:
            """Get streamer navbar for polling (HTMX endpoint)."""
            sort_by_size = bool(request.args.get("sorted", False))
            play_video = request.args.get("play_video", None)
            previous_state = request.args.get("prev_state", None)
            
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            
            streamer_context: Dict[str, Any] = {}
            
            if streamer and not has_error:
                # Only update if state changed
                if previous_state != streamer.sc:
                    streamer_context = get_streamer_context(
                        streamer, sort_by_size, play_video,
                        request.headers.get('User-Agent')
                    )
            
            status_code = 200 if not has_error else 500
            
            context = {
                **streamer_context,
                'update_content': len(streamer_context) > 0,
                'streamer': streamer or InvalidStreamer(user, site),
                'has_error': has_error,
                'refresh_freq': WEB_STATUS_FREQUENCY,
            }
            
            return render_template('streamer_nav_bar.html.jinja', **context), status_code

        @app.route("/streamer-info/<user>/<site>", methods=['GET'])
        @self._login_required
        def get_streamer_info(user: str, site: str) -> Tuple[str, int]:
            """Get streamer info card (HTMX endpoint)."""
            streamer, has_error, error_msg = self._get_streamer_safe(user, site)
            
            if not has_error and streamer:
                try:
                    streamer.cache_file_list()
                except Exception as e:
                    self.logger.error(f"Error caching file list for {user}/{site}: {e}")
            
            status_code = 200 if not has_error else 500
            
            context = {
                'streamer': streamer or InvalidStreamer(user, site),
                'streamer_has_error': has_error,
                'streamer_error_message': error_msg if has_error else None,
                'confirm_deletes': confirm_deletes(request.headers.get('User-Agent')),
            }
            
            return render_template('streamer_record.html.jinja', **context), status_code

        # ====================================================================
        # START SERVER
        # ====================================================================
        
        # Suppress Flask's verbose logging
        log_level = logging.WARNING
        logging.getLogger('werkzeug').setLevel(log_level)
        logging.getLogger('flask').setLevel(log_level)
        
        try:
            self.logger.info(f"Starting web server on {WEBSERVER_HOST}:{WEBSERVER_PORT}")
            app.run(host=WEBSERVER_HOST, port=WEBSERVER_PORT, debug=False, use_reloader=False)
        except Exception as e:
            self.logger.error(f"Failed to start web server: {e}")
            raise