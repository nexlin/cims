#!/usr/bin/env python3
"""
CIMS JSON → MariaDB 데이터 임포트 스크립트

사용법:
    python3 import_data.py [--host HOST] [--port PORT] \
                           [--user USER] [--password PASS] \
                           [--db DB] \
                           [--user-dir USER_DIR] \
                           [--group-dir GROUP_DIR]

기본값:
    host      = 127.0.0.1
    port      = 3306
    user      = root
    password  = (빈 문자열)
    db        = cims
    user-dir  = ../csp/User
    group-dir = ../csp/Group
"""

import argparse
import json
import os
import sys
import glob

try:
    import mysql.connector as mariadb
except ImportError:
    try:
        import pymysql as mariadb
        mariadb.install_as_MySQLdb()
        import mysql.connector as mariadb
    except ImportError:
        print("ERROR: mysql-connector-python 또는 pymysql 패키지가 필요합니다.")
        print("  pip3 install mysql-connector-python")
        sys.exit(1)


def parse_args():
    p = argparse.ArgumentParser(description="CIMS JSON → MariaDB 임포트")
    p.add_argument("--host",      default="127.0.0.1")
    p.add_argument("--port",      type=int, default=3306)
    p.add_argument("--user",      default="root")
    p.add_argument("--password",  default="")
    p.add_argument("--db",        default="cims")
    p.add_argument("--user-dir",  default="../csp/User")
    p.add_argument("--group-dir", default="../csp/Group")
    return p.parse_args()


def connect(args):
    try:
        conn = mariadb.connect(
            host=args.host,
            port=args.port,
            user=args.user,
            password=args.password,
            database=args.db,
            charset="utf8mb4",
        )
        print(f"[DB] Connected to {args.host}:{args.port}/{args.db}")
        return conn
    except Exception as e:
        print(f"[DB] Connection failed: {e}")
        sys.exit(1)


def import_users(conn, user_dir):
    cur = conn.cursor()
    files = glob.glob(os.path.join(user_dir, "*.json"))
    if not files:
        print(f"[User] No JSON files found in {user_dir}")
        return

    count = 0
    for fpath in sorted(files):
        fname = os.path.basename(fpath)
        user_id = fname[:-5]  # 확장자 제거 (.json)

        try:
            with open(fpath, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:
            print(f"[User] Skip {fname}: {e}")
            continue

        auth_id    = data.get("auth_id", user_id)
        passwd     = data.get("passwd", "")
        org_id     = data.get("org_id", "")
        dnd        = 1 if str(data.get("dnd", "false")).lower() == "true" else 0
        forward_id = data.get("forward_id", "")
        create_time = data.get("create_time") or None
        update_time = data.get("update_time") or None

        sql = """
            INSERT INTO csp_users
                (id, auth_id, passwd, org_id, dnd, forward_id, create_time, update_time)
            VALUES (%s, %s, %s, %s, %s, %s, %s, %s)
            ON DUPLICATE KEY UPDATE
                auth_id=VALUES(auth_id), passwd=VALUES(passwd),
                org_id=VALUES(org_id),   dnd=VALUES(dnd),
                forward_id=VALUES(forward_id),
                update_time=VALUES(update_time)
        """
        cur.execute(sql, (user_id, auth_id, passwd, org_id, dnd, forward_id, create_time, update_time))

        # 착신거부 목록
        reject_ids = data.get("reject_id", [])
        if reject_ids:
            cur.execute("DELETE FROM csp_user_rejects WHERE user_id=%s", (user_id,))
            for rid in reject_ids:
                cur.execute(
                    "INSERT IGNORE INTO csp_user_rejects (user_id, reject_id) VALUES (%s, %s)",
                    (user_id, rid)
                )

        count += 1
        print(f"[User] Imported {user_id}  auth_id={auth_id}")

    conn.commit()
    cur.close()
    print(f"[User] Total {count} users imported.\n")


def import_groups(conn, group_dir):
    cur = conn.cursor()
    files = glob.glob(os.path.join(group_dir, "*.json"))
    if not files:
        print(f"[Group] No JSON files found in {group_dir}")
        return

    count = 0
    for fpath in sorted(files):
        fname = os.path.basename(fpath)
        group_id = fname[:-5]

        try:
            with open(fpath, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:
            print(f"[Group] Skip {fname}: {e}")
            continue

        name = data.get("name", group_id)

        cur.execute(
            "INSERT INTO csp_groups (id, name) VALUES (%s, %s) "
            "ON DUPLICATE KEY UPDATE name=VALUES(name)",
            (group_id, name)
        )

        # 멤버 목록 교체
        cur.execute("DELETE FROM csp_group_members WHERE group_id=%s", (group_id,))
        for member in data.get("users", []):
            uid   = member.get("id", "")
            prio  = int(member.get("priority", 0))
            if uid:
                cur.execute(
                    "INSERT INTO csp_group_members (group_id, user_id, priority) VALUES (%s, %s, %s)",
                    (group_id, uid, prio)
                )
                print(f"[Group] {group_id} ← member {uid} (priority={prio})")

        count += 1
        print(f"[Group] Imported group {group_id} ({name})\n")

    conn.commit()
    cur.close()
    print(f"[Group] Total {count} groups imported.")


def main():
    args = parse_args()
    conn = connect(args)

    print("=== 가입자 임포트 ===")
    import_users(conn, args.user_dir)

    print("=== 그룹 임포트 ===")
    import_groups(conn, args.group_dir)

    conn.close()
    print("\n완료.")


if __name__ == "__main__":
    main()
