#!/usr/bin/env python3
"""초기 데이터 CSV 의 가입 번호·IMSI 일괄 재부여 (DB 설치 전, 18 단계 전에 쓴다).

번호 규칙 — 사람(login_id)의 끝 숫자를 순번으로 쓴다. 같은 사람의 PTT/VoLTE 가 같은 순번을
갖게 되어 나중에 사람과 번호를 맞춰 보기 쉽다.
    VoLTE  +8213 + 8자리 순번        PTT  +825 + 8자리 순번
    IMSI   45033 + 번호 숫자(+ 제외)

번호를 바꾸면 **그 번호를 참조하는 CSV 도 같이** 바꿔야 한다 — ptt_group_members.csv 가
PTT 번호를 들고 있다. 여기서 함께 처리한다.

  ./tools/tb-renumber-csv.py            # 미리보기 (아무것도 바꾸지 않는다)
  ./tools/tb-renumber-csv.py --apply    # 반영 (.bak 로 원본 보관)

**18 단계보다 먼저** 돌린다. 18 은 번호를 열쇠로 넣으므로, 이미 넣은 뒤에 바꾸면 옛 행이
남고 새 행이 추가된다 — 그때는 가입자·그룹멤버 표를 비우고 18 을 다시 돌려야 한다:
  sudo mariadb -e "DELETE FROM cims.ptt_group_members; \
                   DELETE FROM cims.ptt_subscriptions; DELETE FROM cims.volte_subscriptions; \
                   DELETE FROM cims.voip_subscriptions;"
  sudo ./tb-install.sh --role db-data
"""
import re, sys, shutil, os

PLMN   = '45033'
PREFIX = {'volte': '+8213', 'voip': '+8221', 'ptt': '+825'}     # 뒤에 8자리 순번이 붙는다 (kind = 가입 테이블)
# tools/ 안에 있으므로 데이터는 한 단계 위의 data/ 다. 키트 밖에서 돌릴 때는
# TB_DATA_DIR 로 지정한다.
_HERE = os.path.dirname(os.path.abspath(__file__))
DATA  = os.environ.get('TB_DATA_DIR') or os.path.join(_HERE, '..', 'data')
DATA  = os.path.normpath(DATA)
if not os.path.isdir(DATA):
    DATA = os.path.normpath(os.path.join(_HERE, 'data'))


def rows(path):
    """주석(#)·빈 줄은 원문 그대로 흘리고, 헤더와 데이터만 파싱한다."""
    out, header = [], None
    for line in open(path, encoding='utf-8').read().splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            out.append(('raw', line)); continue
        if header is None:
            header = line.split(','); out.append(('raw', line)); continue
        out.append(('row', line.split(',')))
    return header, out


def serial(login_id):
    m = re.search(r'(\d+)\s*$', login_id)
    if not m:
        sys.exit(f"ERROR: login_id 끝에 숫자가 없어 순번을 못 만듭니다: {login_id!r}")
    return int(m.group(1))


def main():
    apply = '--apply' in sys.argv
    subs = os.path.join(DATA, 'subscriptions.csv')
    mem  = os.path.join(DATA, 'ptt_group_members.csv')
    if not os.path.isfile(subs):
        sys.exit(f"ERROR: {subs} 가 없습니다 — 키트 안(deployment/tb 또는 ~/cims-tb/tb)에서 실행하세요")

    h, lines = rows(subs)
    idx = {c.strip(): i for i, c in enumerate(h)}
    for need in ('number', 'login_id', 'kind', 'imsi'):
        if need not in idx:
            sys.exit(f"ERROR: subscriptions.csv 에 '{need}' 열이 없습니다")

    mapping, plan = {}, []
    for tag, r in lines:
        if tag != 'row':
            continue
        old   = r[idx['number']].strip()
        login = r[idx['login_id']].strip()
        kind  = r[idx['kind']].strip().lower()
        if kind not in PREFIX:
            sys.exit(f"ERROR: kind 는 ptt|volte|voip 여야 합니다 (number={old}, kind={kind})")
        new  = f"{PREFIX[kind]}{serial(login):08d}"
        imsi = PLMN + new.lstrip('+')
        if new in mapping.values():
            sys.exit(f"ERROR: 새 번호가 중복입니다 {new} — login_id 순번이 겹칩니다")
        mapping[old] = new
        r[idx['number']] = new
        r[idx['imsi']]   = imsi
        plan.append((kind, old, new, imsi))

    print(f"{'서비스':6} {'지금 번호':16} → {'새 번호':16} 새 IMSI")
    for kind, old, new, imsi in plan:
        print(f"{kind:6} {old:16} → {new:16} {imsi}")

    # 참조 CSV — 번호 열만 갈아 끼운다
    mem_out, mem_hit = None, 0
    if os.path.isfile(mem):
        mh, mlines = rows(mem)
        midx = {c.strip(): i for i, c in enumerate(mh)}
        if 'number' not in midx:
            sys.exit("ERROR: ptt_group_members.csv 에 'number' 열이 없습니다")
        for tag, r in mlines:
            if tag != 'row':
                continue
            cur = r[midx['number']].strip()
            if cur in mapping:
                r[midx['number']] = mapping[cur]; mem_hit += 1
            else:
                print(f"  ⚠ ptt_group_members.csv 의 {cur} 는 subscriptions.csv 에 없습니다 — 그대로 둡니다")
        mem_out = mlines
        print(f"\nptt_group_members.csv: 번호 {mem_hit}건 갱신 예정")

    if not apply:
        print("\n미리보기입니다. 반영하려면 --apply 를 붙이세요.")
        return 0

    def write(path, lines):
        shutil.copy2(path, path + '.bak')
        with open(path, 'w', encoding='utf-8') as f:
            for tag, r in lines:
                f.write((r if tag == 'raw' else ','.join(r)) + '\n')

    write(subs, lines)
    if mem_out is not None:
        write(mem, mem_out)
    print(f"\n✓ 반영 완료 — 원본은 *.bak 로 남겼습니다")
    print("  다음: sudo ./tb-install.sh --role db-data   (18 단계 초기 데이터 입력)")
    return 0


sys.exit(main())
