from enum import Enum, auto


class OrmDbType(Enum):
    SQLITE = auto()
    MYSQL = auto()
    ORACLE = auto()
    POSTGRESQL = auto()
