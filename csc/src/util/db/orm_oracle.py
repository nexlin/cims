import pandas as pd
from typing import List, Optional, Sequence, Any, Type
from sqlalchemy import text
from datetime import datetime
from util.scheme_define import FieldDefinition, MetaKeyword


class OrmOracle:

    @staticmethod
    def inspect_columns_without_comment(engine, table_name: str) -> List[FieldDefinition]:
        fields: List[FieldDefinition] = []
        with engine.connect() as conn:
            # Oracle stores table names in uppercase by default usually, but we should respect input
            # However, standard practice is often uppercase. Let's try to match exact first.
            # If table_name is not quoted, it might be case sensitive or not.
            # For simplicity in this fallback, we assume the user provided name matches the DB storage (often UPPERCASE).
            
            # 1. Get Columns
            query_cols = text("""
                SELECT COLUMN_NAME, DATA_TYPE, DATA_PRECISION, DATA_SCALE
                FROM ALL_TAB_COLUMNS
                WHERE TABLE_NAME = :table_name
                ORDER BY COLUMN_ID
            """)
            rows = conn.execute(query_cols, {"table_name": table_name}).fetchall()
            
            # 2. Get PKs
            query_pk = text("""
                SELECT cols.COLUMN_NAME
                FROM ALL_CONSTRAINTS cons, ALL_CONS_COLUMNS cols
                WHERE cols.TABLE_NAME = :table_name
                  AND cons.CONSTRAINT_TYPE = 'P'
                  AND cons.CONSTRAINT_NAME = cols.CONSTRAINT_NAME
                  AND cons.OWNER = cols.OWNER
            """)
            pk_rows = conn.execute(query_pk, {"table_name": table_name}).fetchall()
            pk_cols = set(row[0] for row in pk_rows)

            for row in rows:
                col_name = row[0]
                data_type = row[1].upper()
                precision = row[2]
                scale = row[3]

                python_type = str
                if data_type in ('NUMBER', 'INTEGER', 'SMALLINT'):
                    if scale is not None and scale > 0:
                        python_type = float
                    elif precision is not None and precision > 0:
                        python_type = int
                    else:
                        # NUMBER without precision/scale can be anything, defaulting to float or int?
                        # Usually int if no scale.
                        python_type = int
                elif data_type in ('FLOAT', 'BINARY_FLOAT', 'BINARY_DOUBLE'):
                    python_type = float
                elif data_type in ('DATE', 'TIMESTAMP', 'TIMESTAMP(6)'): 
                    # Oracle TIMESTAMP can have various suffixes
                    python_type = datetime
                elif 'TIMESTAMP' in data_type:
                    python_type = datetime
                
                meta = {}
                if col_name in pk_cols:
                    meta[MetaKeyword.IS_KEY] = True
                fields.append(FieldDefinition(name=col_name, python_type=python_type, metadata=meta))
        return fields

    @staticmethod
    def merge_upsert(
            session,
            model_class: Type[Any],
            df: pd.DataFrame,
            use_columns: Sequence[str],
            pk_columns: Sequence[str],
            *,
            update_policy: str = "all_except_pk",               # policy: 'all_except_pk' | 'only' | 'none'
            update_columns: Optional[Sequence[str]] = None,     # update_policy='only'에서만 사용
            skip_nulls_on_update: bool = False,                 # True면 NULL은 덮어쓰지 않음
    ):
        """
        - pk_columns : ON조건에만 사용, UPDATE 대상에서 자동 제외
        - update_policy:
            * 'all_except_pk' : DF와 테이블 교집합 중 PK 제외 전부 업데이트
            * 'only'          : update_columns에 지정된 컬럼만 업데이트
            * 'none'          : 매칭 시 UPDATE 생략(즉, INSERT-ONLY upsert)
        - skip_nulls_on_update:
            * True면 UPDATE SET에서 각 컬럼을
              t.col = CASE WHEN s.col IS NULL THEN t.col ELSE s.col END
              로 생성해 NULL 덮어쓰기를 방지합니다.
        """
        target_table = model_class.__table__

        if update_policy == "none":
            upd_cols: List[str] = []
        elif update_policy == "only":
            cols = list(update_columns or [])
            upd_cols = [c for c in cols if c in use_columns and c not in pk_columns]
        else:  # 'all_except_pk' (default)
            upd_cols = [c for c in use_columns if c not in pk_columns]

        # 5. create MERGE-SQL
        table_identifier = f"{target_table.schema}.{target_table.name}" if target_table.schema else target_table.name
        select_list = ", ".join([f":{c} AS {c}" for c in use_columns])
        on_clause = " AND ".join([f"t.{pk} = s.{pk}" for pk in pk_columns])
        if upd_cols:
            if skip_nulls_on_update:
                set_exprs = ", ".join([f"t.{c} = CASE WHEN s.{c} IS NULL THEN t.{c} ELSE s.{c} END" for c in upd_cols])
            else:
                set_exprs = ", ".join([f"t.{c} = s.{c}" for c in upd_cols])
            matched_clause = f"WHEN MATCHED THEN UPDATE SET {set_exprs}"
        else:
            matched_clause = ""
        insert_cols = ", ".join(use_columns)
        insert_vals = ", ".join([f"s.{c}" for c in use_columns])
        merge_sql = f"""
                MERGE INTO {table_identifier} t
                USING (SELECT {select_list} FROM dual) s
                ON ({on_clause})
                {matched_clause}
                WHEN NOT MATCHED THEN INSERT ({insert_cols}) VALUES ({insert_vals})
            """

        # 6. executemany
        # for col in df.select_dtypes(include=['object', 'string']):
        #     df[col] = df[col].replace('', ' ', regex=False)
        params = df.to_dict(orient="records")
        session.execute(text(merge_sql), params)
