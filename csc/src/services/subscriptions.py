"""가입 테이블 레지스트리 — 접속환경 kind ↔ 가입 테이블 (sip_service_model.md §2-9, db_schema.md).

테이블 = 접속환경 kind 하나: `volte_subscriptions`(이동 VoLTE) · `voip_subscriptions`(유선 VoIP) · `ptt_subscriptions`(MCPTT).
kind 를 열거하는 곳(관리 API 경로 세그먼트·응답 키·`/provisioning/me` 순회·관제 앱 관리 계약·person 해석)은 전부 여기를
import 한다 — 핸들러마다 튜플을 따로 두지 않는다.

불변식(쓰기 게이트가 지킨다): 가입 행의 `service_ref` 가 가리키는 접속서비스의 kind 는 그 행이 든 테이블의 kind 와 같다
(`kind_matches`). 전화 가족(volte∪voip, `PHONE_KINDS`)은 CSP 전화 경로·로그/통계 축·전화번호부 합산에만 쓰고 테이블
선택·서비스 해석에는 쓰지 않는다.

스키마 프로브: `voip_subscriptions` 는 `sql/migrate_voip_subscriptions.sql` 로 뒤에 생긴 테이블이라 부재 DB 에서도
동작해야 한다 — `has_table`/`tables` 가 SHOW TABLES 로 1회 확인해 프로세스 수명 캐시한다(다른 두 테이블은 기본 스키마).
"""
from __future__ import annotations

from typing import Optional

KINDS = ('volte', 'voip', 'ptt')
PHONE_KINDS = ('volte', 'voip')                 # 전화 가족 — 같은 CSP 전화 경로(B2BUA+CMP relay, TAS)
PTT_ALIASES = ('ptt', 'mcptt')                  # 접속서비스 레코드 kind 의 PTT 별칭

TABLES = {
    'volte': 'volte_subscriptions',
    'voip': 'voip_subscriptions',
    'ptt': 'ptt_subscriptions',
}
# 관리 API `/api/v1/users/{pid}/<segment>` → kind (call 은 이동 VoLTE 의 역사적 세그먼트)
API_SEGMENTS = {'call': 'volte', 'voip': 'voip', 'ptt': 'ptt'}
# 사용자 응답의 배열 키
RESPONSE_KEYS = {'volte': 'call_subscriptions', 'voip': 'voip_subscriptions', 'ptt': 'ptt_subscriptions'}

# 마이그레이션으로 뒤에 생긴 테이블 — 부재 프로브 대상. 값 = 안내할 마이그레이션 파일.
OPTIONAL_TABLES = {'voip': 'sql/migrate_voip_subscriptions.sql'}

SCHEMA_ERROR_VOIP = {'error': 'schema_not_migrated',
                     'detail': 'voip_subscriptions table absent — sql/migrate_voip_subscriptions.sql not applied'}

_present: dict = {}                             # kind → bool (프로세스 수명 캐시)


def reset_probe() -> None:
    """프로브 캐시 초기화 — 시험·재접속용."""
    _present.clear()


def table(kind: str) -> str:
    """kind → 테이블 이름. 모르는 kind 는 KeyError(호출자 검증 뒤에 쓴다)."""
    return TABLES[normalize_kind(kind)]


def normalize_kind(kind: str) -> str:
    k = (kind or '').strip().lower()
    return 'ptt' if k in PTT_ALIASES else k


def kind_of_table(table_name: str) -> str:
    for k, t in TABLES.items():
        if t == table_name:
            return k
    return ''


def kind_matches(kind: str, record_kind: str) -> bool:
    """테이블 kind ↔ 접속서비스 레코드 kind 의 exact 일치(ptt≡mcptt). 레코드 kind 가 비어 있으면 이름만으로 인정한다."""
    rk = normalize_kind(record_kind)
    return not rk or rk == normalize_kind(kind)


def has_table(cur, kind: str) -> bool:
    """kind 의 가입 테이블 존재 여부 — 선택 테이블만 SHOW TABLES 로 확인(1회 캐시), 기본 테이블은 True."""
    k = normalize_kind(kind)
    if k not in OPTIONAL_TABLES:
        return k in TABLES
    if k not in _present:
        cur.execute("SHOW TABLES LIKE %s", (TABLES[k],))
        _present[k] = cur.fetchone() is not None
    return _present[k]


def tables(cur, kinds=KINDS) -> list:
    """존재하는 가입 테이블 [(kind, table)] — 열거 순서는 KINDS(volte, voip, ptt)."""
    return [(k, TABLES[k]) for k in kinds if has_table(cur, k)]


def phone_tables(cur) -> list:
    """전화 가족 테이블 [(kind, table)] — CSP 전화 경로를 타는 회선(volte∪voip)."""
    return tables(cur, PHONE_KINDS)


def phone_union_sql(cur, columns: str = 'id, user_id') -> str:
    """전화 가족 회선을 한 relation 으로 — `(SELECT … FROM volte_subscriptions UNION ALL SELECT … FROM voip_subscriptions)`.
    FROM 절에 alias 를 붙여 쓴다. voip 테이블이 없으면 volte 만."""
    parts = [f"SELECT {columns} FROM {t}" for _k, t in phone_tables(cur)]
    return "(" + " UNION ALL ".join(parts) + ")"


def all_union_sql(cur, columns: str = 'id, user_id') -> str:
    """존재하는 전 가입 테이블의 UNION ALL relation."""
    parts = [f"SELECT {columns} FROM {t}" for _k, t in tables(cur)]
    return "(" + " UNION ALL ".join(parts) + ")"


def find_line(cur, line_id: str) -> Optional[tuple]:
    """회선 id → (kind, table, user_id). 어느 테이블에도 없으면 None. 열거 순서 = KINDS."""
    for k, t in tables(cur):
        cur.execute(f"SELECT user_id FROM {t} WHERE id=%s", (line_id,))
        row = cur.fetchone()
        if row:
            uid = row['user_id'] if isinstance(row, dict) else row[0]
            return k, t, uid
    return None


def number_taken(cur, line_id: str, exclude_kind: str = '') -> Optional[str]:
    """번호 유일성 — 3 테이블 + 대표번호(phone_groups.pilot_id) 주소 공간에서 이미 쓰이면 어디인지(테이블/`phone_groups`) 돌려준다.
    exclude_kind 의 테이블은 건너뛴다(같은 테이블 PK 충돌은 호출자가 따로 본다)."""
    ex = normalize_kind(exclude_kind)
    for k, t in tables(cur):
        if k == ex:
            continue
        cur.execute(f"SELECT 1 FROM {t} WHERE id=%s", (line_id,))
        if cur.fetchone():
            return t
    cur.execute("SHOW TABLES LIKE 'phone_groups'")
    if cur.fetchone():
        cur.execute("SELECT id FROM phone_groups WHERE pilot_id=%s", (line_id,))
        if cur.fetchone():
            return 'phone_groups'
    return None


def parse_sip_transport(value):
    """`sip_transport` 입력 → 'UDP'|'TCP'|'TLS'|None. None/''/'ANY' = NULL(단말 선택). 그 외 ValueError."""
    if value in (None, ''):
        return None
    v = str(value).strip().upper()
    if v == 'ANY':
        return None
    if v not in ('UDP', 'TCP', 'TLS'):
        raise ValueError(v)
    return v


def wire_sip_transport(value) -> str:
    """DB sip_transport → 관제 앱 와이어 값. NULL = 'ANY'."""
    v = (value or '').strip().upper() if isinstance(value, str) else ''
    return v or 'ANY'
