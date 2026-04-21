

if __name__ == '__main__':
    from sqlalchemy import create_engine
    import pandas as pd

    engine = create_engine("oracle+oracledb://cbm:core0908@121.161.164.106:1521/?service_name=ORCLCDB")
    print(engine.dialect.name)  # oracle 이어야 정상
    print(pd.read_sql_query("SELECT 1 FROM DUAL", engine))