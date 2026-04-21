import os
from loguru import logger

from util.singleton import SingletonInstance


class Logger(SingletonInstance):

    def _on_init_once(self, log_dir: str='./', log_file_prefix: str='log_', retention_day: int=30):
        self._logger = logger
        os.makedirs(log_dir, exist_ok=True)
        self._handler = f"{log_dir}/{log_file_prefix}_{{time:YYYY-MM-DD}}.log"
        self._logger.remove()
        self._logger.add(
            self._handler,
            rotation="00:00",                           # 매일 자정에 rotation
            retention=f"{retention_day} days",          # ?일간 로그 보관
            # compression="zip",                        # 오래된 로그 파일 압축
            enqueue=True,                               # 비동기 로깅 활성화
            backtrace=True,                             # 예외 발생 시 상세 트레이스백 기록
            diagnose=True,                              # 예외 발생 시 변수 값 등 추가 정보 기록
            level="INFO"
        )

    def setLogLevel(self, level: str):
        self._logger.configure(handlers=[{"sink": self._handler, "level": level}])

    def log_error(self, log_str: str, *args, **kwargs):
        self._logger.opt(depth=1).error(log_str, *args, **kwargs)

    def log_warning(self, log_str: str, *args, **kwargs):
        self._logger.opt(depth=1).warning(log_str, *args, **kwargs)

    def log_info(self, log_str: str, *args, **kwargs):
        self._logger.opt(depth=1).info(log_str, *args, **kwargs)

    def log_verbose(self, log_str: str, *args, **kwargs):
        self._logger.opt(depth=1).debug(log_str, *args, **kwargs)


class ModuleLogger(SingletonInstance):

    def _on_init_once(self):
        self._logger = logger
        self._module_handler = {}
        self._module_logger = {}

    def addModule(self,
                  module: str,
                  log_dir: str='./',
                  log_file_prefix: str='log_',
                  retention_day: int=30):
        os.makedirs(log_dir, exist_ok=True)
        handler = f"{log_dir}/{log_file_prefix}_{{time:YYYY-MM-DD}}.log"
        self._logger.add(
            handler,
            rotation="00:00",                       # 매일 자정에 rotation
            retention=f"{retention_day} days",      # ?일간 로그 보관
            # compression="zip",                    # 오래된 로그 파일 압축
            enqueue=True,                           # 비동기 로깅 활성화
            backtrace=True,                         # 예외 발생 시 상세 트레이스백 기록
            diagnose=True,                          # 예외 발생 시 변수 값 등 추가 정보 기록
            level="INFO",
            filter=lambda record: record["extra"].get("module") == module
        )
        self._module_handler[module] = handler
        self._module_logger[module] = logger.bind(module=module)

    def setLogLevel(self, module: str, level: str):
        module_handler = self._module_handler.get(module, None)
        if module_handler:
            self._logger.configure(handlers=[{"sink": module_handler, "level": level}])

    def log_error(self, module: str, log_str: str, *args, **kwargs):
        module_logger = self._module_logger.get(module, None)
        if module_logger:
            module_logger.opt(depth=1).error(log_str, *args, **kwargs)

    def log_warning(self, module: str, log_str: str, *args, **kwargs):
        module_logger = self._module_logger.get(module, None)
        if module_logger:
            module_logger.opt(depth=1).warning(log_str, *args, **kwargs)

    def log_info(self, module: str, log_str: str, *args, **kwargs):
        module_logger = self._module_logger.get(module, None)
        if module_logger:
            module_logger.opt(depth=1).info(log_str, *args, **kwargs)

    def log_verbose(self, module: str, log_str: str, *args, **kwargs):
        module_logger = self._module_logger.get(module, None)
        if module_logger:
            module_logger.opt(depth=1).debug(log_str, *args, **kwargs)