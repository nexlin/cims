from typing import List
from sqlalchemy import text
from datetime import datetime
from util.scheme_define import FieldDefinition, MetaKeyword


class OrmMysql:

    @staticmethod
    def inspect_columns_without_comment(engine, table_name: str) -> List[FieldDefinition]:
        fields: List[FieldDefinition] = []
        with engine.connect() as conn:
            db_name = engine.url.database
            query = text("""
                SELECT COLUMN_NAME, DATA_TYPE, COLUMN_KEY, COLUMN_TYPE
                FROM information_schema.COLUMNS
                WHERE TABLE_SCHEMA = :schema AND TABLE_NAME = :table
                ORDER BY ORDINAL_POSITION
            """)
            rows = conn.execute(query, {"schema": db_name, "table": table_name}).fetchall()

            pk_cols = set()
            for row in rows:
                if row[2] == 'PRI':
                    pk_cols.add(row[0])

            for row in rows:
                col_name = row[0]
                data_type = row[1].lower()
                col_type = row[3].lower()

                python_type = str
                if data_type in ('int', 'bigint', 'smallint', 'mediumint', 'tinyint'):
                    if data_type == 'tinyint' and 'tinyint(1)' in col_type:
                        python_type = bool
                    else:
                        python_type = int
                elif data_type in ('float', 'double', 'decimal', 'numeric'):
                    python_type = float
                elif data_type in ('datetime', 'timestamp', 'date', 'time'):
                    python_type = datetime
                elif data_type == 'boolean':
                    python_type = bool

                meta = {}
                if col_name in pk_cols:
                    meta[MetaKeyword.IS_KEY] = True
                fields.append(FieldDefinition(name=col_name, python_type=python_type, metadata=meta))
        return fields