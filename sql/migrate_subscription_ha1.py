#!/usr/bin/env python3
"""ha1 일괄 이행 — 기존 passwd + 서비스 realm 으로 H(A1) 을 계산해 채운다 (멱등).

    H(A1) = MD5( <imsi>@<service.domain> ":" <realm> ":" <passwd> )
    realm = access_services.auth_realm ?? domain   (CSP CspServiceMap::EffectiveRealm 과 동일)

서비스 정의(domain/auth_realm)는 DB 가 아니라 OAM 스토어(services.json)에 있으므로 두 경로로 받는다:
  --services-json <path>   OAM 스토어의 access_services.jsonl (dist: build/dist/config/access_services.jsonl)
                           또는 같은 행들의 JSON 배열 (name/domain/auth_realm)
  --service NAME=DOMAIN[:REALM]   개별 지정 (반복 가능). REALM 생략 시 DOMAIN.

대상 = ha1='' AND passwd<>'' AND imsi 비어있지 않음 AND service_ref 가 해석되는 행.
이미 ha1 이 있는 행은 건드리지 않는다 (2회 실행 = no-op).

사용:
  python3 sql/migrate_subscription_ha1.py --db-json build/dist/csp/config/csp.json \\
      --services-json build/dist/config/access_services.jsonl [--dry-run]
"""
import argparse
import hashlib
import json
import sys


def _load_db_cfg(path):
    with open(path, encoding='utf-8') as f:
        cfg = json.load(f)
    # csp.json: {"Setup": {"Database": {Host, Port, User, Password, DbName}}} (cspsim -db 와 동일 해석)
    db = (cfg.get('Setup') or {}).get('Database') or cfg.get('Database') or cfg
    return {
        'host': db.get('Host', '127.0.0.1'),
        'port': int(db.get('Port', 3306)),
        'user': db.get('User', 'cims'),
        'password': db.get('Password', ''),
        'database': db.get('DbName', 'cims'),
    }


def _load_services(args):
    realms = {}
    if args.services_json:
        with open(args.services_json, encoding='utf-8') as f:
            text = f.read()
        if args.services_json.endswith('.jsonl'):
            rows = [json.loads(line) for line in text.splitlines() if line.strip()]
        else:
            data = json.loads(text)
            rows = (data.get('rows') or data.get('items') or data) if isinstance(data, dict) else data
        for r in rows:
            name = r.get('name')
            if not name:
                continue
            realms[name] = (r.get('domain') or '', r.get('auth_realm') or r.get('domain') or '')
    for spec in args.service or []:
        name, _, rest = spec.partition('=')
        domain, _, realm = rest.partition(':')
        realms[name] = (domain, realm or domain)
    return realms


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--db-json', required=True, help='DB 접속 정보를 담은 JSON (csp/csp.json 형식)')
    ap.add_argument('--services-json', help='services.json (name/domain/auth_realm)')
    ap.add_argument('--service', action='append', help='NAME=DOMAIN[:REALM] (반복 가능)')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    realms = _load_services(args)
    if not realms:
        sys.exit('ERROR: 서비스 realm 정보가 없습니다 (--services-json 또는 --service)')

    import pymysql
    conn = pymysql.connect(**_load_db_cfg(args.db_json), charset='utf8mb4', autocommit=False)
    total_done = 0
    try:
        with conn.cursor() as cur:
            for table in ('volte_subscriptions', 'ptt_subscriptions'):
                cur.execute(f"SELECT id, COALESCE(imsi,''), COALESCE(service_ref,''), passwd FROM {table} "
                            "WHERE ha1='' AND passwd<>''")
                rows = cur.fetchall()
                done = skipped = 0
                for sid, imsi, sref, passwd in rows:
                    if not imsi or sref not in realms:
                        skipped += 1
                        continue
                    domain, realm = realms[sref]
                    ha1 = hashlib.md5(f"{imsi}@{domain}:{realm}:{passwd}".encode('utf-8')).hexdigest()
                    if not args.dry_run:
                        cur.execute(f"UPDATE {table} SET ha1=%s WHERE id=%s AND ha1=''", (ha1, sid))
                    done += 1
                print(f"{table}: 대상 {len(rows)} / 계산 {done} / 건너뜀(imsi·service_ref 미해석) {skipped}")
                total_done += done
        if args.dry_run:
            conn.rollback()
            print(f"dry-run — 변경 없음 (계산 가능 {total_done})")
        else:
            conn.commit()
            print(f"완료 — ha1 {total_done} 행 채움")
    finally:
        conn.close()


if __name__ == '__main__':
    main()
