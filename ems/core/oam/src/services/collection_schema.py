"""컬렉션 SoT 의 모듈/버전 정합 — runtime store v2 P5.

문제: 컬렉션 SoT 는 버전 무관(modules/<owner>/runtime/collections/<name>)인데
config_template 의 schema 는 모듈 버전 종속. 모듈 v0.1.0→v0.2.0 에서 컬렉션 schema
가 바뀌면(필드 추가/제거/이름변경) 기존 SoT 레코드가 신버전 모양과 어긋난다.

해결:
  - 각 컬렉션 SoT 에 schema_version(.schema_version 파일) 스탬프.
  - 모듈 업그레이드(package 버전 전환) 시 SoT 를 신버전 schema 로 마이그레이션:
      * 일반(generic): 신 schema 에 없는 필드 제거 + 누락 필드 default 채움.
      * 커스텀: register_schema_migration(collection, fn) 로 rename/split 등 등록.
  - 멱등: SoT 의 schema_version 이 이미 대상 버전이면 skip.
"""
import os

from services import file_store, ha_lookup

_SV_FILE = '.schema_version'

# 커스텀 마이그레이션 훅 — fn(records, old_tmpl, new_tmpl) -> records'
_SCHEMA_MIGRATIONS: dict = {}


def register_schema_migration(collection: str, fn) -> None:
    """컬렉션별 커스텀 마이그레이션 등록(필드 rename/split 등 generic 으로 불가한 변환)."""
    _SCHEMA_MIGRATIONS[collection] = fn


def get_schema_version(cdir: str):
    try:
        with open(os.path.join(cdir, _SV_FILE), encoding='utf-8') as f:
            return f.read().strip()
    except (FileNotFoundError, OSError):
        return None


def set_schema_version(cdir: str, version) -> None:
    os.makedirs(cdir, exist_ok=True)
    tmp = os.path.join(cdir, _SV_FILE + '.tmp')
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(str(version))
    os.replace(tmp, os.path.join(cdir, _SV_FILE))


def _schema_fields(tmpl: dict, name: str):
    """template.collections[key=name].schema.fields → field dict 리스트."""
    for c in (tmpl or {}).get('collections', []):
        if c.get('key') == name:
            return c.get('schema', {}).get('fields', []) or []
    return None


def align_record(record: dict, fields: list) -> dict:
    """레코드를 신 schema 필드 집합에 정렬 — 누락 필드 default 채움 + 미정의 필드 제거.
    (id 등 내부키, '_' prefix 메타 필드는 보존.)"""
    allowed = {f.get('key') for f in fields if f.get('key')}
    defaults = {f['key']: f.get('default') for f in fields if f.get('key')}
    out = {}
    for k, v in record.items():
        if k in allowed or k.startswith('_') or k in ('id', 'create_time', 'update_time'):
            out[k] = v
    for k in allowed:
        if k not in out:
            out[k] = defaults.get(k)
    return out


def migrate_module_collections(config: dict, owner: str,
                               old_tmpl: dict, new_tmpl: dict, new_version) -> list:
    """owner 모듈의 모든 컬렉션 SoT 를 new_tmpl schema(버전 new_version)로 정합.
    SoT 가 이미 new_version 이면 skip(멱등). 마이그레이션한 컬렉션명 리스트 반환."""
    migrated = []
    for c in (new_tmpl or {}).get('collections', []):
        name = c.get('key')
        if not name:
            continue
        if (ha_lookup._COLLECTION_OWNER.get(name) or owner) != owner:
            continue
        cdir = ha_lookup.collection_dir(config, name, create=False)
        if not os.path.isdir(cdir):
            continue  # 데이터 없음 — 마이그레이션 불요
        if str(get_schema_version(cdir)) == str(new_version):
            continue  # 이미 정합
        rows = file_store.load_all(cdir)
        new_fields = _schema_fields(new_tmpl, name) or []
        hook = _SCHEMA_MIGRATIONS.get(name)
        if hook:
            rows = hook(rows, old_tmpl, new_tmpl)
        else:
            rows = [align_record(r, new_fields) for r in rows]
        for r in rows:
            rid = r.get('id')
            if rid is not None:
                file_store.save(cdir, rid, r)
        set_schema_version(cdir, new_version)
        migrated.append(name)
    return migrated
