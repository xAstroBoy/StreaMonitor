
import logging
from typing import Optional
import parameters
import sys

# Initialize colorama for Windows color support if available
try:
    from colorama import init, Fore, Back, Style
    init(autoreset=True)
    COLORAMA_AVAILABLE = True
except ImportError:
    COLORAMA_AVAILABLE = False


class ColoredFormatter(logging.Formatter):
    """Custom formatter that gets colors from the bot instance."""
    
    def __init__(self, logger_name: str, bot_instance=None):
        super().__init__('%(asctime)s - %(levelname)s - {}: %(message)s'.format(logger_name))
        self.bot_instance = bot_instance
    
    def format(self, record):
        # Get colors from bot if available
        if self.bot_instance and hasattr(self.bot_instance, 'get_site_color'):
            try:
                color, attrs = self.bot_instance.get_site_color()
            except:
                color, attrs = ("white", [])
        else:
            color, attrs = ("white", [])
        
        from termcolor import colored
        
        # Color the timestamp with different time parts
        formatted_time = self.formatTime(record, self.datefmt)
        # Split time into date and time parts for better coloring
        date_part, time_part = formatted_time.split(' ', 1)
        colored_time = colored(date_part, "dark_grey") + " " + colored(time_part, "cyan", attrs=["bold"])
        
        # Color the log level based on level type
        level_colors = {
            'DEBUG': ("white", []),
            'INFO': ("green", ["bold"]),
            'WARNING': ("yellow", ["bold"]),
            'ERROR': ("red", ["bold"]),
            'CRITICAL': ("red", ["bold", "underline"])
        }
        level_color, level_attrs = level_colors.get(record.levelname, ("white", []))
        colored_level = colored(record.levelname.ljust(8), level_color, attrs=level_attrs)
        
        # Color the logger name (site + username) with site color and bold
        logger_name_part = self._style._fmt.split(': %(message)s')[0].split(' - ')[-1]
        colored_logger_name = colored(logger_name_part, color, attrs=["bold"] + attrs)
        
        # Color the message with site color but lighter
        colored_message = colored(record.getMessage(), color, attrs=attrs)
        
        # Create the final formatted message with better spacing
        return f"{colored_time} - {colored_level} - {colored_logger_name}: {colored_message}"


class Logger:
    def __init__(self, name: str = "__name__", bot_instance=None) -> None:
        self.name = name
        self.bot_instance = bot_instance
        self.formatter = ColoredFormatter(name, bot_instance)
        self.handler: Optional[logging.StreamHandler] = None
        
        # Avoid duplicate handlers
        self.logger = logging.getLogger(self.name)
        if not self.logger.handlers:
            self.handler = logging.StreamHandler(sys.stdout)
            self.handler.setFormatter(self.formatter)
            
            loglevel = logging.DEBUG if parameters.DEBUG else logging.INFO
            self.logger.setLevel(loglevel)
            self.logger.addHandler(self.handler)

    def get_logger(self) -> logging.Logger:
        """Get the configured logger instance."""
        logger = logging.getLogger(self.name)
        
        # Only add handler if not already present
        if not logger.handlers and self.handler:
            logger.setLevel(logging.DEBUG if parameters.DEBUG else logging.INFO)
            logger.addHandler(self.handler)
        
        return logger

    def debug(self, msg: str) -> None:
        self.logger.debug(msg)

    def warning(self, msg: str) -> None:
        self.logger.warning(msg)

    def error(self, msg: str) -> None:
        self.logger.error(msg)

    def info(self, msg: str) -> None:
        self.logger.info(msg)

    def exception(self, msg: str) -> None:
        self.logger.exception(msg)

    def critical(self, msg: str) -> None:
        self.logger.critical(msg)
    
    def setLevel(self, level: int) -> None:
        """Set the logging level for this logger."""
        self.logger.setLevel(level)
        if self.handler:
            self.handler.setLevel(level)

    def verbose(self, msg: str) -> None:
        if parameters.DEBUG:
            self.logger.debug(msg)

    def set_level(self, level: int) -> None:
        self.logger.setLevel(level)