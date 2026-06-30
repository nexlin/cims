import threading
import queue
from typing import TypeVar, Any, Generic, Callable

QMsgT = TypeVar("QMsgT")
QUEUE_HANDLER = Callable[[QMsgT], Any]


class MessageQueueWorker(threading.Thread, Generic[QMsgT]):
    def __init__(self, handler:QUEUE_HANDLER, timeout:int = 1):
        super().__init__(daemon=True)
        self._queue: "queue.Queue[QMsgT]" = queue.Queue()
        self._handler = handler
        self._timeout = timeout
        self._stop_event = threading.Event()

    def run(self):
        while not self._stop_event.is_set():
            try:
                msg = self._queue.get(timeout=self._timeout)
            except queue.Empty:
                continue

            try:
                self._handler(msg)
            finally:
                self._queue.task_done()

    def put(self, msg: QMsgT):
        self._queue.put(msg)

    def stop(self):
        self._stop_event.set()

    def wait_until_done(self):
        self._queue.join()
