
COMMON_OPTS = {
    "echo", "future", "connect_args", "isolation_level",
    "pool_size", "max_overflow", "pool_timeout",
    "pool_recycle", "pool_pre_ping", "pool_use_lifo"
}

DIALECT_OPTS = {
    "mysql": {
        "encoding"
    },
    "postgresql": {
        "executemany_mode",
        "executemany_batch_page_size",
        "executemany_values_page_size",
    },
    "sqlite": {
        # SQLite는 대부분 pool 옵션 사용 불가
        "check_same_thread",
    },
    "oracle": {
        "encoding",
        "nencoding",
    },
    "mssql": {
        "fast_executemany",
    },
}


def filterEngineOptions(db_type: str, **options) -> dict:
    allowed = COMMON_OPTS | DIALECT_OPTS.get(db_type.lower(), set())
    filtered = {k: v for k, v in options.items() if k in allowed}
    return filtered