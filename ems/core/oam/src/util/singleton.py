import threading

class SingletonInstance:
    _instances = {}
    _lock = threading.Lock()

    def __new__(cls, *args, **kwargs):
        if cls not in cls._instances:
            with cls._lock:
                if cls not in cls._instances:
                    instance = super().__new__(cls)
                    cls._instances[cls] = instance
        return cls._instances[cls]

    def __init__(self, *args, **kwargs):
        if not hasattr(self, "_initialized"):
            self._initialized = True
            self._on_init_once(*args, **kwargs)

    def _on_init_once(self, *args, **kwargs):
        pass
