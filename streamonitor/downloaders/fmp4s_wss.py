# streamonitor/downloaders/wssvr.py
# Fixed WebSocket-based VR video downloader

import json
import os
import subprocess
import time
from threading import Thread, Lock, Event
from websocket import create_connection, WebSocketConnectionClosedException, WebSocketException
from contextlib import closing

from parameters import DEBUG, CONTAINER, SEGMENT_TIME, FFMPEG_PATH
from streamonitor.bot import Bot


def getVideoWSSVR(self: 'Bot', url: str, filename: str) -> bool:
    """
    Download video from WebSocket VR stream.
    Returns True on success, False on failure.
    """
    
    # Thread-safe state management
    stop_event = Event()
    error_lock = Lock()
    error_state = {'occurred': False, 'message': None}
    
    # Convert protocol
    url = url.replace('fmp4s://', 'wss://')
    
    # Build filenames with optional suffix
    suffix = ''
    if hasattr(self, 'filename_extra_suffix'):
        suffix = self.filename_extra_suffix
    
    basefilename = filename[:-len('.' + CONTAINER)]
    final_filename = basefilename + suffix + '.' + CONTAINER
    tmpfilename = basefilename + '.tmp.mp4'
    
    def set_error(message):
        """Thread-safe error setting."""
        with error_lock:
            error_state['occurred'] = True
            error_state['message'] = message
            self.logger.error(f"WebSocket download error: {message}")
    
    def debug_(message):
        """Debug logging helper."""
        if DEBUG:
            try:
                self.debug(message, final_filename + '.log')
            except Exception as e:
                self.logger.warning(f"Failed to write debug log: {e}")
    
    def execute():
        """Download thread main function."""
        bytes_written = 0
        reconnect_count = 0
        max_reconnects = 10
        
        try:
            with open(tmpfilename, 'wb') as outfile:
                while not stop_event.is_set() and reconnect_count < max_reconnects:
                    try:
                        debug_(f"Connecting to WebSocket (attempt {reconnect_count + 1})...")
                        
                        with closing(create_connection(url, timeout=10)) as conn:
                            # Send hello message
                            conn.send('{"url":"stream/hello","version":"0.0.1"}')
                            
                            # Wait for quality negotiation
                            negotiated = False
                            for _ in range(5):  # Max 5 attempts
                                if stop_event.is_set():
                                    return
                                
                                try:
                                    t = conn.recv()
                                    
                                    # Try to parse as JSON
                                    try:
                                        tj = json.loads(t)
                                        
                                        if 'url' in tj and tj['url'] == 'stream/qual':
                                            conn.send('{"quality":"test","url":"stream/play","version":"0.0.1"}')
                                            debug_('Connection established, quality negotiated')
                                            negotiated = True
                                            break
                                        
                                        if 'message' in tj and tj['message'] == 'ping':
                                            debug_('Server not ready or stream changed')
                                            set_error('Server sent ping - stream not available')
                                            return
                                    
                                    except (json.JSONDecodeError, ValueError):
                                        # Not JSON, might be binary data already
                                        debug_('Received non-JSON message during negotiation')
                                        continue
                                
                                except WebSocketException as e:
                                    debug_(f'Error during negotiation: {e}')
                                    break
                            
                            if not negotiated:
                                debug_('Failed to negotiate quality, reconnecting...')
                                reconnect_count += 1
                                time.sleep(2)
                                continue
                            
                            # Reset reconnect counter on successful negotiation
                            reconnect_count = 0
                            
                            # Download binary data
                            stall_check_time = time.monotonic()
                            last_data_time = time.monotonic()
                            
                            while not stop_event.is_set():
                                try:
                                    # Receive with timeout
                                    conn.settimeout(10)
                                    data = conn.recv()
                                    
                                    if not data:
                                        debug_('Received empty data, connection may be closing')
                                        break
                                    
                                    # Write binary data
                                    if isinstance(data, bytes):
                                        outfile.write(data)
                                        bytes_written += len(data)
                                        last_data_time = time.monotonic()
                                    else:
                                        # Might be a text message
                                        try:
                                            tj = json.loads(data)
                                            debug_(f'Received message during download: {tj}')
                                            
                                            # Handle control messages
                                            if 'message' in tj:
                                                if tj['message'] == 'ping':
                                                    debug_('Server ping - stream may have ended')
                                                    return
                                                elif tj['message'] == 'end':
                                                    debug_('Server signaled end of stream')
                                                    return
                                        except (json.JSONDecodeError, ValueError):
                                            debug_(f'Received non-binary, non-JSON data: {data[:100]}')
                                    
                                    # Check for stall every 30 seconds
                                    now = time.monotonic()
                                    if now - stall_check_time > 30:
                                        if now - last_data_time > 45:
                                            debug_(f'No data received for {int(now - last_data_time)}s, reconnecting...')
                                            break
                                        stall_check_time = now
                                
                                except WebSocketException as e:
                                    debug_(f'WebSocket error during download: {e}')
                                    break
                    
                    except WebSocketConnectionClosedException:
                        debug_('WebSocket connection closed, attempting reconnect...')
                        reconnect_count += 1
                        time.sleep(2)
                        continue
                    
                    except WebSocketException as wex:
                        debug_(f'WebSocket exception: {wex}')
                        reconnect_count += 1
                        if reconnect_count >= max_reconnects:
                            set_error(f'Max reconnection attempts reached: {wex}')
                            return
                        time.sleep(2)
                        continue
                    
                    except Exception as e:
                        debug_(f'Unexpected error: {e}')
                        set_error(f'Unexpected error: {e}')
                        return
                
                if reconnect_count >= max_reconnects:
                    set_error('Exceeded maximum reconnection attempts')
        
        except OSError as e:
            set_error(f'Failed to open temp file: {e}')
        except Exception as e:
            set_error(f'Unexpected error in download thread: {e}')
        finally:
            debug_(f'Download thread ending. Bytes written: {bytes_written}')
    
    def terminate():
        """Stop the download gracefully."""
        stop_event.set()
    
    # Start download thread
    process = Thread(target=execute, daemon=True)
    process.start()
    self.stopDownload = terminate
    
    # Wait for completion
    process.join()
    self.stopDownload = None
    
    # Check for errors
    with error_lock:
        if error_state['occurred']:
            self.logger.error(f"Download failed: {error_state['message']}")
            # Clean up temp file
            try:
                if os.path.exists(tmpfilename):
                    os.remove(tmpfilename)
                    self.logger.debug(f"Cleaned up temp file: {tmpfilename}")
            except Exception as e:
                self.logger.warning(f"Failed to clean up temp file: {e}")
            return False
    
    # Check if temp file exists and has content
    if not os.path.exists(tmpfilename):
        self.logger.error("Temp file does not exist after download")
        return False
    
    try:
        file_size = os.path.getsize(tmpfilename)
        if file_size == 0:
            self.logger.error("Downloaded file is empty")
            os.remove(tmpfilename)
            return False
        self.logger.info(f"Downloaded {file_size:,} bytes to temp file")
    except Exception as e:
        self.logger.error(f"Failed to check temp file: {e}")
        return False
    
    # Post-process with FFmpeg
    try:
        self.logger.info("Post-processing with FFmpeg...")
        
        # Build FFmpeg command
        cmd = [FFMPEG_PATH, '-hide_banner', '-loglevel', 'error']
        
        # CRITICAL FIX: Add fflags to ignore broken timestamps
        cmd.extend(['-fflags', '+igndts+genpts'])
        
        # Input
        cmd.extend(['-i', tmpfilename, '-ignore_editlist', '1'])
        
        # Stream copy
        cmd.extend(['-c:a', 'copy', '-c:v', 'copy'])
        
        # Timestamp handling - avoid_negative_ts only (no reset_timestamps with igndts+genpts)
        cmd.extend(['-avoid_negative_ts', 'make_zero'])
        
        # Output format
        if SEGMENT_TIME is not None:
            # Segmented output
            cmd.extend([
                '-f', 'segment',
                '-segment_time', str(SEGMENT_TIME)
            ])
            
            # Set segment format based on container
            if CONTAINER == 'mp4':
                cmd.extend(['-segment_format', 'mp4'])
                cmd.extend(['-segment_format_options', 'movflags=frag_keyframe+empty_moov'])
            elif CONTAINER == 'mkv':
                cmd.extend(['-segment_format', 'matroska'])
            else:
                cmd.extend(['-segment_format', 'mpegts'])
            
            output_pattern = basefilename + '_%03d' + suffix + '.' + CONTAINER
            cmd.append(output_pattern)
        else:
            # Single file output
            if CONTAINER == 'mp4':
                # MP4 needs special flags (no faststart for compatibility)
                cmd.extend(['-movflags', 'frag_keyframe+empty_moov'])
            
            cmd.append(final_filename)
        
        # Prepare stdout/stderr
        if DEBUG:
            stdout_log = open(final_filename + '.postprocess_stdout.log', 'w')
            stderr_log = open(final_filename + '.postprocess_stderr.log', 'w')
        else:
            stdout_log = subprocess.DEVNULL
            stderr_log = subprocess.DEVNULL
        
        try:
            # Run FFmpeg
            result = subprocess.run(
                cmd,
                stdout=stdout_log,
                stderr=stderr_log,
                timeout=300  # 5 minute timeout for post-processing
            )
            
            # Check result
            if result.returncode not in (0, 255):
                self.logger.error(f"FFmpeg post-processing failed with code {result.returncode}")
                return False
            
            self.logger.info("Post-processing completed successfully")
        
        finally:
            # Close log files
            if DEBUG:
                try:
                    stdout_log.close()
                    stderr_log.close()
                except Exception:
                    pass
        
        # Remove temp file
        try:
            os.remove(tmpfilename)
            self.logger.debug("Cleaned up temp file after post-processing")
        except Exception as e:
            self.logger.warning(f"Failed to remove temp file: {e}")
    
    except subprocess.TimeoutExpired:
        self.logger.error("FFmpeg post-processing timed out")
        return False
    except FileNotFoundError:
        self.logger.error(f"FFmpeg not found at: {FFMPEG_PATH}")
        return False
    except Exception as e:
        self.logger.error(f"Post-processing error: {e}")
        # Try to clean up temp file
        try:
            if os.path.exists(tmpfilename):
                os.remove(tmpfilename)
        except Exception:
            pass
        return False
    
    return True