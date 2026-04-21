import pandas as pd
import asyncio
from contextlib import contextmanager
from typing import Any, Type, Optional, Dict, List, Sequence, Tuple
from datetime import datetime
from sqlalchemy import create_engine, inspect, text, select
from sqlalchemy.orm import sessionmaker, scoped_session, DeclarativeBase, declarative_base, Mapped, mapped_column
from sqlalchemy.sql import func, sqltypes
from sqlalchemy.dialects import postgresql, mysql
from sqlalchemy.dialects import sqlite
from strenum import StrEnum

from util.db.orm_def import OrmDbType
from util.db import orm_param
from util.db.orm_oracle import OrmOracle
from util.db.orm_mysql import OrmMysql
from util.scheme_define import SchemaDefinition, MetaKeyword, FieldDefinition


class TimeStampKwd(StrEnum):
    DEFAULT_NOW = 'now'


class OrmProc:

    DEFAULT_CHUNK_SIZE = 2000

    def __init__(self, db_type: str, user: str, passwd: str, ip: str, port: int, db: str, **kwargs):
        self._base_repository: Type[DeclarativeBase] = declarative_base()
        self._db_url = self._get_url(db_type, user, passwd, ip, port, db, **kwargs)
        filter_param_dict = orm_param.filterEngineOptions(db_type, **kwargs)
        self._engine = create_engine(self._db_url, **filter_param_dict)
        self._session_factory = sessionmaker(bind=self._engine, autoflush=True, expire_on_commit=True)
        self._session = scoped_session(self._session_factory)
        self._registered_models: Dict[str, Tuple[List[str], Type[Any]]] = {} # key: table_name, value: (pk_columns, model_class)

    @staticmethod
    def _pythonType2ormType(python_type: type):
        if python_type is int:
            return sqltypes.Integer
        elif python_type is float:
            return sqltypes.Float
        elif python_type is bool:
            return sqltypes.Boolean
        elif python_type is dict:
            return sqltypes.JSON
        elif python_type is datetime:
            return sqltypes.TIMESTAMP
        else:
            return sqltypes.String(255)

    @staticmethod
    def _ormType2pythonType(t) -> Type:
        if isinstance(t, (sqltypes.Integer, sqltypes.BigInteger, sqltypes.SmallInteger)):
            return int
        if isinstance(t, (sqltypes.Float, sqltypes.Numeric, sqltypes.DECIMAL, sqltypes.REAL)):
            return float
        if isinstance(t, sqltypes.Boolean):
            return bool
        if isinstance(t, (sqltypes.DateTime, sqltypes.TIMESTAMP, sqltypes.Date, sqltypes.Time)):
            return datetime
        return str

    @staticmethod
    def _get_url(db_type: str, user: str, passwd: str, ip: str, port: int, db: str, **kwargs) -> str:
        if db_type.upper() == OrmDbType.MYSQL.name:
            # sudo apt-get install python3-dev default-libmysqlclient-dev build-essential
            # pip install mysqlclient
            char_set = kwargs.get('charset', 'utf8mb4')
            return f"mysql+mysqldb://{user}:{passwd}@{ip}:{port}/{db}?charset={char_set}"
        elif db_type.upper() == OrmDbType.ORACLE.name:
            # pip install oracledb
            return f"oracle+oracledb://{user}:{passwd}@{ip}:{port}/?service_name={db}"
        else:
            raise ValueError(f"OrmProc : unknown db type : {db_type}")

    @staticmethod
    def _convert_df_to_model_types(model_class: Type[Any], df: pd.DataFrame) -> pd.DataFrame:
        out = df.copy()
        model_columns_info = {c.name: c.type for c in model_class.__table__.columns}
        for model_column_name, model_column_type in model_columns_info.items():
            if model_column_name not in out.columns:
                continue
            if isinstance(model_column_type, (sqltypes.TIMESTAMP, sqltypes.DATETIME, sqltypes.DATE)):
                out[model_column_name] = pd.to_datetime(out[model_column_name])
        return out

    @staticmethod
    def _prepare_df_for_set(model_class: Type[Any],
                            df: pd.DataFrame,
                            pk_columns: Sequence[str],
                            *,
                            drop_rows_missing_pk: bool = True) \
            -> Tuple[pd.DataFrame, List[str]]:
        # 1. check pk_columns
        missing_pk = [pk for pk in pk_columns if pk not in df.columns]
        if missing_pk:
            raise ValueError(f"OrmProc._prepare_df_for_set : missing pk({missing_pk}) from dataframe")

        # 2. table_columns && df_columns
        table_cols = [col.name for col in model_class.__table__.columns]
        use_cols = [c for c in df.columns if c in table_cols]
        if not use_cols:
            raise ValueError("OrmProc._prepare_df_for_set : not match columns")

        for pk in pk_columns:
            if pk not in use_cols:
                raise ValueError(f"OrmProc._prepare_df_for_set : missing pk({missing_pk}) from use_cols")

        # 3. NaN -> None
        df2 = df[use_cols].where(pd.notnull(df[use_cols]), None)

        # 4. drop row when PK is None
        if pk_columns and drop_rows_missing_pk:
            df2 = df2.dropna(subset=pk_columns, how='any')

        return df2, use_cols

    @contextmanager
    def _get_read_session(self):
        session = self._session()
        try:
            yield session
        except Exception as e:
            raise e
        finally:
            self._session.remove()

    @contextmanager
    def _get_write_session(self):
        session = self._session()
        try:
            yield session
            session.commit()
        except Exception as e:
            session.rollback()
            raise e
        finally:
            self._session.remove()

    def _register_orm_model(self, schema: SchemaDefinition):
        # prevention of duplicate registration to base repository
        if schema.name in self._registered_models:
            return
        else:
            attributes = {
                '__tablename__': schema.name,
                '__annotations__': {},
            }

            if not any(f.metadata.get(MetaKeyword.IS_KEY) for f in schema.fields):
                attributes['__annotations__']['id'] = Mapped[int]
                attributes['id'] = mapped_column(sqltypes.Integer, primary_key=True, autoincrement=True)

            pk_columns = []
            for field in schema.fields:
                sqlalchemy_type = self._pythonType2ormType(field.python_type)
                attributes['__annotations__'][field.name] = Mapped[Optional[field.python_type]]
                column_kwargs = {}
                # IS_KEY
                if field.metadata.get(MetaKeyword.IS_KEY):
                    column_kwargs['primary_key'] = True
                    pk_columns.append(field.name)
                else:
                    column_kwargs['nullable'] = True
                # DEFAULT
                default_value = field.metadata.get(MetaKeyword.DEFAULT)
                if default_value is not None:
                    column_kwargs['nullable'] = False
                    if sqlalchemy_type is sqltypes.TIMESTAMP:
                        if default_value == TimeStampKwd.DEFAULT_NOW:
                            column_kwargs['server_default'] = func.now()
                        elif isinstance(default_value, str) and default_value:
                            column_kwargs['server_default'] = text(f"TIMESTAMP '{default_value}'")
                        else:
                            column_kwargs['server_default'] = func.now()
                    else:
                        column_kwargs['server_default'] = text(str(field.metadata.get(MetaKeyword.DEFAULT)))
                # PARAMETER
                if field.metadata.get(MetaKeyword.PARAMETER):
                    pass
                attributes[field.name] = mapped_column(sqlalchemy_type, **column_kwargs)

            dynamic_model = type(schema.name, (self._base_repository,), attributes)
            # self._base_repository.registry.mapped_as_dataclass(dynamic_model)
            self._registered_models[schema.name] = (pk_columns, dynamic_model)

    def _insert_data(self, schema_name: str, data: pd.DataFrame, chunk_size: int):
        with self._get_write_session() as session:
            conn = session.get_bind()
            data.to_sql(
                name=schema_name,
                con=conn,
                if_exists='append',
                index=False,
                method='multi',
                chunksize=chunk_size
            )

    def _upsert_from_dataframe(self,
                               df: pd.DataFrame,
                               chunk_size: int,
                               model_class: Type[Any],
                               pk_columns: Sequence[str],
                               commit_per_chunk: bool = False):
        prepared_df, use_cols = self._prepare_df_for_set(model_class, df, pk_columns)
        with self._get_write_session() as session:
            size = chunk_size if chunk_size > 0 else self.DEFAULT_CHUNK_SIZE
            dialect_name = self._engine.dialect.name.upper()
            for start in range(0, len(prepared_df), size):
                sub_df = prepared_df.iloc[start:start + size]
                if dialect_name == OrmDbType.POSTGRESQL.name:
                    data_to_upsert = sub_df.to_dict(orient='records')
                    stmt = postgresql.insert(model_class).values(data_to_upsert)
                    update_cols = {
                        col: getattr(stmt.excluded, col)
                        for col in use_cols if col not in pk_columns
                    }
                    final_stmt = stmt.on_conflict_do_update(index_elements=pk_columns, set_=update_cols)
                    session.execute(final_stmt)
                elif dialect_name == OrmDbType.SQLITE.name:
                    data_to_upsert = sub_df.to_dict(orient='records')
                    stmt = sqlite.insert(model_class).values(data_to_upsert)
                    update_cols = {
                        col: getattr(stmt.excluded, col)
                        for col in use_cols if col not in pk_columns
                    }
                    final_stmt = stmt.on_conflict_do_update(index_elements=pk_columns, set_=update_cols)
                    session.execute(final_stmt)
                elif dialect_name == OrmDbType.MYSQL.name:
                    data_to_upsert = sub_df.to_dict(orient='records')
                    stmt = mysql.insert(model_class).values(data_to_upsert)
                    update_cols = {
                        col: getattr(stmt.inserted, col)
                        for col in use_cols if col not in pk_columns
                    }
                    final_stmt = stmt.on_duplicate_key_update(update_cols)
                    session.execute(final_stmt)
                elif dialect_name == OrmDbType.ORACLE.name:
                    OrmOracle.merge_upsert(session, model_class, sub_df, use_cols, pk_columns)

                if commit_per_chunk:
                    session.commit()

    def createSchemaFromTable(self, table_name: str) -> SchemaDefinition:
        fields: List[FieldDefinition] = []
        try:
            ins = inspect(self._engine)
            cols = ins.get_columns(table_name)
            pk = ins.get_pk_constraint(table_name)
            pk_cols = set(pk.get('constrained_columns') or [])
            for col in cols:
                python_type = self._ormType2pythonType(col["type"])
                meta = {}
                if col["name"] in pk_cols:
                    meta[MetaKeyword.IS_KEY] = True
                fields.append(FieldDefinition(name=col["name"], python_type=python_type, metadata=meta))
        except Exception as e:
            if self._engine.dialect.name == 'mysql':
                fields = OrmMysql.inspect_columns_without_comment(self._engine, table_name)
            elif self._engine.dialect.name == 'oracle':
                fields = OrmOracle.inspect_columns_without_comment(self._engine, table_name)
            else:
                raise e
        return SchemaDefinition(name=table_name, fields=fields)

    def createTableFromSchema(self, schema: SchemaDefinition):
        try:
            self._register_orm_model(schema)
            table_to_create = self._base_repository.metadata.tables[schema.name]
            self._base_repository.metadata.create_all(self._engine, tables=[table_to_create], checkfirst=True)
        except Exception as e:
            raise e

    def create_all_registered_tables(self):
        try:
            self._base_repository.metadata.create_all(self._engine)
        except Exception as e:
            raise e

    def drop_table(self, schema: SchemaDefinition):
        try:
            tbl = self._base_repository.metadata.tables.get(schema.name)
            if tbl is not None:
                tbl.drop(self._engine, checkfirst=True)  # 실제 드롭
                self._base_repository.metadata.remove(tbl)  # 메타에서 제거
        except Exception as e:
            raise e

    def set_data(self, schema_name: str, df: pd.DataFrame, chunk_size: int = DEFAULT_CHUNK_SIZE, enable_upsert: bool = False):
        if df.empty:
            raise ValueError(f"OrmProc : set_data : dataframe is empty")

        model_info = self._registered_models.get(schema_name)
        if not model_info:
            raise ValueError(f"OrmProc : set_data : {schema_name} is not registered")
        pk_columns = model_info[0]
        model_class = model_info[1]
        convert_df = self._convert_df_to_model_types(model_class, df)
        if pk_columns and enable_upsert:
            self._upsert_from_dataframe(convert_df, chunk_size, model_class, pk_columns)
        else:
            self._insert_data(schema_name, convert_df, chunk_size)

    def get_data(self, schema_name: str, filters: Optional[Dict[str, Any]] = None, columns: Optional[List[str]] = None) -> pd.DataFrame:
        """
        :param schema_name: 등록된 모델 이름 (테이블 이름)
        :param filters: {컬럼명: 값} 형태로 WHERE 조건 지정 가능
        :param columns: 조회할 컬럼 리스트. None이면 모든 컬럼 조회
        :return: pandas DataFrame
        """
        model_info = self._registered_models.get(schema_name)
        if not model_info:
            raise ValueError(f"OrmProc : get_data : {schema_name} is not registered")
        _, model_class = model_info

        try:
            if columns:
                selected_columns = [getattr(model_class, c) for c in columns]
                stmt = select(*selected_columns)
            else:
                stmt = select(model_class)

            if filters:
                for col, val in filters.items():
                    col_expr = getattr(model_class, col)
                    if isinstance(val, (list, tuple, set)):
                        stmt = stmt.where(col_expr.in_(list(val)))
                    elif val is None:
                        stmt = stmt.where(col_expr.is_(None))
                    else:
                        stmt = stmt.where(col_expr == val)
        except AttributeError as e:
            raise ValueError(f"Invalid column name provided in filters or columns: {e}")

        with self._get_read_session() as session:
            df = pd.read_sql_query(stmt, session.bind)
            return df

    def inspect_table_names(self, schema: Optional[str] = None) -> List[str]:
        with self._get_read_session() as session:
            inspector = inspect(session.bind)
            table_list = inspector.get_table_names(schema=schema)
            view_list = inspector.get_view_names(schema=schema)
            return table_list + view_list

    async def get_data_async(self,
                             schema_name: str,
                             filters: Optional[Dict[str, Any]] = None,
                             columns: Optional[List[str]] = None) -> pd.DataFrame:
        # @@@@@@ only python version < 3.9
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, self.get_data, schema_name, filters, columns)
        # return await asyncio.to_thread(self.get_data, schema_name, filters, columns)
