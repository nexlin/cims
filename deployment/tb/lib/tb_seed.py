#!/usr/bin/env python3
"""tb_seed.py — TB 초기 데이터 직접 입력 (조직·가입자·가입번호·PTT 그룹).

CSV 를 읽어 DB 에 바로 넣는다. OAM·CSC 가 떠 있지 않아도 되고 콘솔 조작도 필요 없다 —
폐쇄망에서 스키마 생성 직후 곧바로 데이터를 채우는 경로다.

**규약은 콘솔·CSC 가 만드는 행과 동일하게 맞춘다** (실 DB 행을 기준으로 확인):
  organizations   code(유일) · code_path = 부모경로 + '/' + code (루트는 code, org.py:105)
  users           login_id(유일) · org_id = organizations.**code** 문자열 · passwd 평문
                  (IdMS 로그인 자격. CSC 가 LOGIN_ACCOUNTS 로 그대로 비교 — mcptt.py:485)
  *_subscriptions id = 가입 번호(내선/msisdn, PK) · user_id → users.id
                  **ha1 = MD5("<imsi>@<domain>:<realm>:<passwd>")** (admin.py:555)
                  평문 passwd 는 저장하지 않는다 (컬럼이 없다)
  ptt_groups      mcptt_group_id(유일) · org_code = 조직 code
  ptt_group_members  (group_id, user_id) PK — user_id 는 **가입 번호**(문자열)

ha1 의 domain/realm 은 CSP `access_services` 의 그 서비스 행과 **반드시 같아야 한다**.
어긋나면 CSP 가 계산하는 realm 과 달라져 등록이 401 로 실패한다 (initial_install.md §4.2).
realm 을 비우면 domain 을 쓴다 (CSP EffectiveRealm 규칙과 동일).

멱등: 자연키로 upsert 한다. 두 번 돌려도 행이 늘지 않고, 값이 바뀐 것만 갱신된다.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load_pymysql():
    """반입본 vendor → 모듈 vendor 순으로 순수 파이썬 pymysql 을 찾는다 (폐쇄망 — pip 없음)."""
    try:
        import pymysql  # noqa
        return pymysql
    except ImportError:
        pass
    cands = [
        os.path.join(_HERE, '..', 'offline', 'db-bootstrap', 'vendor'),
        os.path.join(_HERE, '..', '..', '..', 'ems', 'core', 'oam', 'vendor'),
        os.path.join(_HERE, '..', '..', '..', 'csc', 'vendor'),
        '/opt/cims-agent/modules/oam/current/oam/vendor',
    ]
    for c in cands:
        if os.path.isdir(os.path.join(c, 'pymysql')):
            sys.path.insert(0, os.path.abspath(c))
            import pymysql  # noqa
            return pymysql
    sys.exit("ERROR: pymysql 을 찾을 수 없습니다 (반입본 offline/db-bootstrap/vendor 확인)")


# ── CSV ────────────────────────────────────────────────────────────────────────
def read_csv(path):
    """헤더 있는 CSV → dict 목록. '#' 로 시작하는 줄과 빈 줄은 건너뛴다."""
    if not os.path.isfile(path):
        return None
    rows = []
    with open(path, encoding='utf-8-sig', newline='') as f:
        lines = [ln for ln in f if ln.strip() and not ln.lstrip().startswith('#')]
    for r in csv.DictReader(lines):
        r = {(k or '').strip(): (v or '').strip() for k, v in r.items() if k}
        if any(r.values()):
            rows.append(r)
    return rows


def need(row, key, where):
    v = (row.get(key) or '').strip()
    if not v:
        sys.exit(f"ERROR: {where} — '{key}' 는 필수입니다: {row}")
    return v


def yn(row, key, default=None):
    """빈 값이면 default(=DB 기본값 사용 의도면 None)."""
    v = (row.get(key) or '').strip().lower()
    if not v:
        return default
    return 1 if v in ('1', 'y', 'yes', 'true', 'on') else 0


def digest_ha1(imsi, domain, realm, passwd):
    return hashlib.md5(f"{imsi}@{domain}:{realm}:{passwd}".encode('utf-8')).hexdigest()


class Stat:
    def __init__(self):
        self.n = {}

    def add(self, table, what):
        self.n.setdefault(table, {'추가': 0, '갱신': 0, '유지': 0})[what] += 1

    def report(self):
        for t, c in self.n.items():
            print(f"  {t:22} 추가 {c['추가']:3}  갱신 {c['갱신']:3}  유지 {c['유지']:3}")


# ── 각 엔티티 ──────────────────────────────────────────────────────────────────
def seed_orgs(cur, rows, st):
    """부모를 먼저 넣어야 code_path 가 잡힌다 — parent_code 참조 순서로 위상 정렬한다."""
    remaining = list(rows)
    done = set()
    cur.execute("SELECT code FROM organizations")
    done |= {r[0] for r in cur.fetchall()}

    progressed = True
    while remaining and progressed:
        progressed = False
        for row in list(remaining):
            code = need(row, 'code', 'organizations.csv')
            parent = (row.get('parent_code') or '').strip()
            if parent and parent not in done:
                continue
            name = need(row, 'name', 'organizations.csv')
            sort_order = int(row.get('sort_order') or 0)

            parent_id, parent_path = None, ''
            if parent:
                cur.execute("SELECT id, code_path FROM organizations WHERE code=%s", (parent,))
                p = cur.fetchone()
                if not p:
                    sys.exit(f"ERROR: organizations.csv — parent_code '{parent}' 를 찾을 수 없습니다")
                parent_id, parent_path = p[0], (p[1] or '')
            code_path = (parent_path + '/' + code) if parent_path else code

            cur.execute("SELECT id, name, parent_id, code_path, sort_order FROM organizations WHERE code=%s", (code,))
            cur_row = cur.fetchone()
            if cur_row is None:
                cur.execute("INSERT INTO organizations (code, code_path, name, parent_id, sort_order) "
                            "VALUES (%s,%s,%s,%s,%s)", (code, code_path, name, parent_id, sort_order))
                st.add('organizations', '추가')
            elif (cur_row[1], cur_row[2], cur_row[3], cur_row[4]) != (name, parent_id, code_path, sort_order):
                cur.execute("UPDATE organizations SET name=%s, parent_id=%s, code_path=%s, sort_order=%s "
                            "WHERE code=%s", (name, parent_id, code_path, sort_order, code))
                st.add('organizations', '갱신')
            else:
                st.add('organizations', '유지')
            done.add(code)
            remaining.remove(row)
            progressed = True

    if remaining:
        bad = ', '.join(r.get('code', '?') for r in remaining)
        sys.exit(f"ERROR: organizations.csv — parent_code 를 해석할 수 없는 행: {bad} (순환 참조?)")


def seed_users(cur, rows, st):
    for row in rows:
        login_id = need(row, 'login_id', 'users.csv')
        name = need(row, 'name', 'users.csv')
        passwd = row.get('passwd') or None
        org = row.get('org_code') or ''
        title = row.get('title') or ''
        email = row.get('email') or ''

        if org:
            cur.execute("SELECT 1 FROM organizations WHERE code=%s", (org,))
            if not cur.fetchone():
                sys.exit(f"ERROR: users.csv — org_code '{org}' 가 organizations 에 없습니다 (login_id={login_id})")

        cur.execute("SELECT id, name, passwd, org_id, title, email FROM users WHERE login_id=%s", (login_id,))
        cur_row = cur.fetchone()
        if cur_row is None:
            cur.execute("INSERT INTO users (name, login_id, passwd, org_id, title, email, create_time, update_time) "
                        "VALUES (%s,%s,%s,%s,%s,%s,NOW(),NOW())",
                        (name, login_id, passwd, org, title, email))
            st.add('users', '추가')
        else:
            want = (name, passwd if passwd is not None else cur_row[2], org, title, email)
            if (cur_row[1], cur_row[2], cur_row[3], cur_row[4], cur_row[5]) != want:
                cur.execute("UPDATE users SET name=%s, passwd=%s, org_id=%s, title=%s, email=%s, update_time=NOW() "
                            "WHERE login_id=%s", (*want, login_id))
                st.add('users', '갱신')
            else:
                st.add('users', '유지')


_KIND_TABLE = {'ptt': 'ptt_subscriptions', 'volte': 'volte_subscriptions', 'voip': 'voip_subscriptions'}   # 가입 테이블 = 접속환경 kind


def seed_subs(cur, rows, st, svc):
    """svc = {kind: (domain, realm)} — ha1 결박 재료."""
    has_pickup = {}
    for row in rows:
        number = need(row, 'number', 'subscriptions.csv')
        kind = need(row, 'kind', 'subscriptions.csv').lower()
        table = _KIND_TABLE.get(kind)
        if not table:
            sys.exit(f"ERROR: subscriptions.csv — kind 는 ptt|volte 여야 합니다 (number={number}, kind={kind})")
        if kind not in svc:
            sys.exit(f"ERROR: {kind} 도메인이 설정되지 않았습니다 — tb-site.conf 의 "
                     f"TB_{kind.upper()}_DOMAIN 을 채우세요 (ha1 유도에 필요)")
        domain, realm = svc[kind]

        login_id = need(row, 'login_id', 'subscriptions.csv')
        cur.execute("SELECT id FROM users WHERE login_id=%s", (login_id,))
        u = cur.fetchone()
        if not u:
            sys.exit(f"ERROR: subscriptions.csv — login_id '{login_id}' 가 users 에 없습니다 (number={number})")
        user_id = u[0]

        imsi = need(row, 'imsi', 'subscriptions.csv')
        service_ref = need(row, 'service_ref', 'subscriptions.csv')
        passwd = need(row, 'passwd', 'subscriptions.csv')
        transport = (row.get('sip_transport') or '').strip().upper() or None
        if transport and transport not in ('UDP', 'TCP', 'TLS'):
            sys.exit(f"ERROR: subscriptions.csv — sip_transport 는 UDP|TCP|TLS (number={number})")
        ha1 = digest_ha1(imsi, domain, realm, passwd)
        pickup = (row.get('pickup_group') or '').strip() or None

        if table not in has_pickup:
            cur.execute(f"SHOW COLUMNS FROM {table} LIKE 'pickup_group'")
            has_pickup[table] = cur.fetchone() is not None
        if pickup and not has_pickup[table]:
            print(f"  ⚠ {table}.pickup_group 컬럼이 없어 무시 (number={number}) — "
                  f"sql/migrate_subscription_pickup_group.sql 미적용")
            pickup = None

        cols = "imsi, service_ref, ha1, sip_transport"
        sel = f"SELECT user_id, {cols}" + (", pickup_group" if has_pickup[table] else "")
        cur.execute(f"{sel} FROM {table} WHERE id=%s", (number,))
        cur_row = cur.fetchone()
        want = (user_id, imsi, service_ref, ha1, transport) + ((pickup,) if has_pickup[table] else ())

        if cur_row is None:
            names = f"id, user_id, {cols}" + (", pickup_group" if has_pickup[table] else "")
            ph = ', '.join(['%s'] * (len(want) + 1))
            cur.execute(f"INSERT INTO {table} ({names}) VALUES ({ph})", (number, *want))
            st.add(table, '추가')
        elif tuple(cur_row) != want:
            sets = "user_id=%s, imsi=%s, service_ref=%s, ha1=%s, sip_transport=%s" \
                   + (", pickup_group=%s" if has_pickup[table] else "")
            cur.execute(f"UPDATE {table} SET {sets} WHERE id=%s", (*want, number))
            st.add(table, '갱신')
        else:
            st.add(table, '유지')


_GROUP_OPT = {  # CSV 열 → (컬럼, 변환). 빈 값이면 DB 기본값을 쓴다.
    'group_type':          ('group_type', str),
    'floor_policy':        ('floor_policy', str),
    'max_talkers':         ('max_talkers', int),
    'priority':            ('priority', int),
    'max_members':         ('max_members', int),
    'require_affiliation': ('require_affiliation', 'bool'),
    'emergency_call':      ('emergency_call', 'bool'),
    'emergency_alert':     ('emergency_alert', 'bool'),
    'allow_sds':           ('allow_sds', 'bool'),
    'video_enabled':       ('video_enabled', 'bool'),
}


def seed_groups(cur, rows, st):
    for row in rows:
        gid = need(row, 'mcptt_group_id', 'ptt_groups.csv')
        name = row.get('name') or gid
        org = (row.get('org_code') or '').strip() or None

        # CSV 에서 값을 준 열만 다룬다 — 빈 칸은 "DB 기본값/현재값 유지" 다.
        extra = {}
        for csv_col, (db_col, conv) in _GROUP_OPT.items():
            raw = (row.get(csv_col) or '').strip()
            if not raw:
                continue
            extra[db_col] = yn(row, csv_col) if conv == 'bool' else conv(raw)

        cols = ['name', 'org_code'] + list(extra)
        cur.execute(f"SELECT {', '.join(cols)} FROM ptt_groups WHERE mcptt_group_id=%s", (gid,))
        cur_row = cur.fetchone()
        want = (name, org) + tuple(extra.values())

        if cur_row is None:
            cur.execute(f"INSERT INTO ptt_groups (mcptt_group_id, {', '.join(cols)}) "
                        f"VALUES ({', '.join(['%s'] * (len(want) + 1))})", (gid, *want))
            st.add('ptt_groups', '추가')
        elif tuple(cur_row) != want:
            sets = ', '.join(f"{c}=%s" for c in cols)
            cur.execute(f"UPDATE ptt_groups SET {sets} WHERE mcptt_group_id=%s", (*want, gid))
            st.add('ptt_groups', '갱신')
        else:
            st.add('ptt_groups', '유지')


def seed_members(cur, rows, st):
    for row in rows:
        gid = need(row, 'mcptt_group_id', 'ptt_group_members.csv')
        number = need(row, 'number', 'ptt_group_members.csv')
        role = (row.get('role') or 'participant').strip().lower()
        if role not in ('chair', 'participant'):
            sys.exit(f"ERROR: ptt_group_members.csv — role 은 chair|participant (number={number})")
        # 빈 칸이면 미지정 — 추가 시 DB 기본값, 갱신 시 현재값을 그대로 둔다.
        raw_prio = (row.get('priority') or '').strip()
        priority = int(raw_prio) if raw_prio else None

        cur.execute("SELECT id FROM ptt_groups WHERE mcptt_group_id=%s", (gid,))
        g = cur.fetchone()
        if not g:
            sys.exit(f"ERROR: ptt_group_members.csv — 그룹 '{gid}' 이 없습니다")
        cur.execute("SELECT 1 FROM ptt_subscriptions WHERE id=%s", (number,))
        if not cur.fetchone():
            sys.exit(f"ERROR: ptt_group_members.csv — 가입번호 '{number}' 가 ptt_subscriptions 에 없습니다 "
                     f"(멤버는 PTT 가입번호여야 합니다)")

        cur.execute("SELECT role, priority FROM ptt_group_members WHERE group_id=%s AND user_id=%s",
                    (g[0], number))
        cur_row = cur.fetchone()
        if cur_row is None:
            cols, vals = ['group_id', 'user_id', 'role'], [g[0], number, role]
            if priority is not None:
                cols.append('priority'); vals.append(priority)
            cur.execute(f"INSERT INTO ptt_group_members ({', '.join(cols)}) "
                        f"VALUES ({', '.join(['%s'] * len(vals))})", vals)
            st.add('ptt_group_members', '추가')
        else:
            same = cur_row[0] == role and (priority is None or cur_row[1] == priority)
            if not same:
                sets, vals = ['role=%s'], [role]
                if priority is not None:
                    sets.append('priority=%s'); vals.append(priority)
                cur.execute(f"UPDATE ptt_group_members SET {', '.join(sets)} "
                            f"WHERE group_id=%s AND user_id=%s", (*vals, g[0], number))
                st.add('ptt_group_members', '갱신')
            else:
                st.add('ptt_group_members', '유지')


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="TB 초기 데이터 직접 입력 (CSV → DB)")
    ap.add_argument('--data-dir', required=True, help='CSV 디렉토리')
    ap.add_argument('--host', default='127.0.0.1'); ap.add_argument('--port', type=int, default=3306)
    ap.add_argument('--db', default='cims'); ap.add_argument('--user', default='cims')
    ap.add_argument('--ptt-domain', default=''); ap.add_argument('--ptt-realm', default='')
    ap.add_argument('--volte-domain', default=''); ap.add_argument('--volte-realm', default='')
    ap.add_argument('--dry-run', action='store_true',
                    help='계획만 보여준다 — 트랜잭션으로 넣어보고 마지막에 되돌린다(반영 없음)')
    ap.add_argument('--check-only', action='store_true',
                    help='입력하지 않고 현재 행수와 ha1 결손만 점검한다 (결손이 있으면 종료코드 2)')
    args = ap.parse_args()

    # 비밀번호는 argv 에 싣지 않는다 (/proc/<pid>/cmdline 노출).
    passwd = os.environ.get('TB_DB_APP_PASS', '')
    if not passwd:
        sys.exit("ERROR: TB_DB_APP_PASS 환경변수가 비어 있습니다")

    pymysql_mod = _load_pymysql()

    if args.check_only:
        cn = pymysql_mod.connect(host=args.host, port=args.port, user=args.user,
                                 password=passwd, database=args.db, charset='utf8mb4')
        bad = 0
        with cn.cursor() as cur:
            for t in ('organizations', 'users', 'ptt_subscriptions', 'volte_subscriptions', 'voip_subscriptions',
                      'ptt_groups', 'ptt_group_members'):
                cur.execute(f"SELECT COUNT(*) FROM {t}")
                print(f"   {t:22} {cur.fetchone()[0]}행")
            # ha1 이 비면 그 번호는 등록이 401 로 실패한다 — 조용히 넘어가지 않게 여기서 잡는다.
            for t in ('ptt_subscriptions', 'volte_subscriptions', 'voip_subscriptions'):
                cur.execute(f"SELECT COUNT(*) FROM {t} WHERE ha1='' OR ha1 IS NULL")
                n = cur.fetchone()[0]
                if n:
                    print(f"   ⚠ {t}: ha1 이 빈 행 {n}건 — 이 번호는 등록에 실패합니다")
                    bad += n
        cn.close()
        raise SystemExit(2 if bad else 0)

    svc = {}
    if args.ptt_domain:
        svc['ptt'] = (args.ptt_domain, args.ptt_realm or args.ptt_domain)
    if args.volte_domain:
        svc['volte'] = (args.volte_domain, args.volte_realm or args.volte_domain)

    d = args.data_dir
    files = {n: read_csv(os.path.join(d, f'{n}.csv'))
             for n in ('organizations', 'users', 'subscriptions', 'ptt_groups', 'ptt_group_members')}
    if all(v is None for v in files.values()):
        sys.exit(f"ERROR: {d} 에 CSV 가 없습니다 (organizations.csv 등)")

    print(f"── 입력 파일 ({d})")
    for n, rows in files.items():
        print(f"   {n + '.csv':26} {'없음' if rows is None else str(len(rows)) + '행'}")
    for k, (dom, rlm) in svc.items():
        print(f"   {k} 도메인/realm: {dom} / {rlm}")
    if args.dry_run:
        print("   ** dry-run — 아무것도 쓰지 않는다 **")

    cn = pymysql_mod.connect(host=args.host, port=args.port, user=args.user,
                         password=passwd, database=args.db, charset='utf8mb4', autocommit=False)
    st = Stat()
    try:
        with cn.cursor() as cur:
            # dry-run 도 같은 경로로 쓴다 — 마지막에 rollback 한다(전 테이블 InnoDB).
            # 그래야 조직 → 사용자 → 가입번호 → 그룹멤버 연쇄 참조가 신규 설치에서도 검증된다.
            if files['organizations']:
                seed_orgs(cur, files['organizations'], st)
            if files['users']:
                seed_users(cur, files['users'], st)
            if files['subscriptions']:
                seed_subs(cur, files['subscriptions'], st, svc)
            if files['ptt_groups']:
                seed_groups(cur, files['ptt_groups'], st)
            if files['ptt_group_members']:
                seed_members(cur, files['ptt_group_members'], st)
        if args.dry_run:
            cn.rollback()
            print("   (dry-run — 모두 되돌렸습니다)")
        else:
            cn.commit()
    except Exception:
        cn.rollback()
        raise
    finally:
        cn.close()

    print("\n── 결과")
    st.report()


if __name__ == '__main__':
    main()
