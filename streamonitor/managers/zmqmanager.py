import zmq
from streamonitor.manager import Manager
import streamonitor.log as log
import zmq.error


class ZMQManager(Manager):
    def __init__(self, streamers):
        super().__init__(streamers)
        self.logger = log.Logger("manager_zmq")

    def run(self):
        ctx = zmq.Context.instance()
        socket = ctx.socket(zmq.REP)

        try:
            socket.bind("tcp://*:6969")
            self.logger.info("[ZMQ] Listening on tcp://*:6969")
        except zmq.error.ZMQError as e:
            if e.errno == zmq.EADDRINUSE:
                self.logger.warning("[ZMQ] Port 6969 already in use, disabling ZMQ manager")
                socket.close()
                return
            else:
                self.logger.error(f"[ZMQ] Fatal error: {e}")
                socket.close()
                return

        try:
            while True:
                line = socket.recv_string()
                self.logger.info("[ZMQ] " + line)
                reply = self.execCmd(line)
                if isinstance(reply, str):
                    socket.send_string(reply)
                else:
                    socket.send_string('')
        finally:
            socket.close()
            ctx.term()
