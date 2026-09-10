#!/usr/bin/env bash
# =============================================================
# 70-callcheck.sh — 호시험 (cspsim PTT 그룹호 / VoLTE 호)
#
# 초도 설치 판정의 최소 확인이다 (매뉴얼 §6.1). 절차의 정본은 VERIFICATION_MANUAL.md.
#
# 시험 대상은 ptt / volte / both 를 고른다. 같은 cspsim 바이너리에 -mode 만 달리 주며,
# -db 에 넘긴 가입자 목록에서 그 모드(kind)에 맞는 행만 골라 로드한다.
#
# 통과 판정은 stdout 만으로 하지 않는다 — PTT 는 `Call OK` 인데 floor 가 안 돌아간 경우가
# 있어 세션 이력의 turn_count 까지 본다. VoLTE 는 즉시 조회되는 이력 API 가 없어(통계는
# 롤업 주기에 걸린다) cspsim 요약을 판정 근거로 쓰고 통계는 참고로만 본다.
#
# -local_ip 를 반드시 명시한다: 생략하면 auto-detect 가 첫 global IP 를 골라 SDP 광고
# 주소와 실제 패킷 출발지가 달라지고, 등록·호는 되는데 floor 만 실패한다.
#
# 시험 조건(대상·단말수·유지시간·그룹)은 **매 실행마다 다시 묻는다** — 설치 값과 달리
# 실행마다 바꾸는 값이다. 사이트 값(주소·도메인·DB)은 묻지 않는다: 도메인을 바꾸면
# 저장된 H(A1) 과 어긋나 등록이 401 로 실패한다(18 단계가 도메인으로 ha1 을 결박한다).
# 사이트 값을 바꿔야 하면 --reconfigure 로 실행한다.
# =============================================================
set -euo pipefail

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/tb-common.sh
source "$_HERE/../lib/tb-common.sh"

tb_load_site
tb_init_dirs
tb_require_python

header "=== [70] 호시험 ==="

# ── 시험 조건 — 매번 묻는다 ──────────────────────────────────
tb_ask_always TB_CALL_KIND     "시험 대상 (ptt / volte / both)" "both"
case "$TB_CALL_KIND" in
    ptt|volte|both) ;;
    *) die "TB_CALL_KIND 는 ptt · volte · both 중 하나여야 합니다 (현재: $TB_CALL_KIND)" ;;
esac
tb_ask_always TB_CALL_COUNT    "동시 단말 수" "4"
tb_ask_always TB_CALL_DURATION "호 유지 시간(초)" "10"
[[ "$TB_CALL_KIND" != volte ]] && \
    tb_ask_always TB_PTT_GROUP "시험할 PTT 그룹 (mcptt_group_id)" "testgrp01"

# ── 사이트 값 — 묻지 않는다 (없으면 그때만 묻는다) ──────────
tb_ask TB_OAM_URL        "OAM 주소" "https://127.0.0.1:4419"
tb_ask TB_ADMIN_PASS     "콘솔 admin 비밀번호" "" secret
tb_ask TB_INSTALL_PREFIX "설치 경로" "/opt/cims-agent"
tb_ask TB_SIP_IP         "SIP 주소" ""
[[ "$TB_CALL_KIND" != volte ]] && tb_ask TB_PTT_DOMAIN   "PTT 도메인" ""
[[ "$TB_CALL_KIND" != ptt   ]] && tb_ask TB_VOLTE_DOMAIN "VoLTE 도메인" ""
tb_ask TB_DB_HOST     "DB 호스트" "127.0.0.1"
tb_ask TB_DB_PORT     "DB 포트" "3306"
tb_ask TB_DB_NAME     "DB 이름" "cims"
tb_ask TB_DB_APP_USER "DB 계정" "cims"
tb_ask TB_DB_APP_PASS "DB 비밀번호" "" secret

# 묻지 않은 값도 무엇이 쓰이는지는 보여준다 — 도메인 불일치는 401 로만 드러나서 찾기 어렵다.
info "사용할 사이트 값 (바꾸려면 --reconfigure)"
echo "        SIP 주소   $TB_SIP_IP"
[[ "$TB_CALL_KIND" != volte ]] && echo "        PTT 도메인   ${TB_PTT_DOMAIN:-}"
[[ "$TB_CALL_KIND" != ptt   ]] && echo "        VoLTE 도메인 ${TB_VOLTE_DOMAIN:-}"
echo "        DB         $TB_DB_APP_USER@$TB_DB_HOST:$TB_DB_PORT/$TB_DB_NAME"

# ── cspsim 찾기 ───────────────────────────────────────────────
sim=""
for c in "$TB_INSTALL_PREFIX/modules/cspsim/current/cspsim/bin/cspsim" \
         "$TB_OFFLINE_DIR/bin/cspsim"; do
    [[ -x "$c" ]] && { sim="$c"; break; }
done
[[ -n "$sim" ]] || die_hint "cspsim 을 찾을 수 없습니다" \
    "cspsim 패키지를 등록·설치하거나(30·40 단계에 cspsim 추가)," \
    "빌드 장비의 build/bin/cspsim 을 반입본 offline/bin/ 에 넣으세요."
ok "cspsim: $sim"

# 미디어 디렉토리 — 없으면 코덱이 PCMU 로 내려가 판정이 흐려진다.
media=""
for c in "$(dirname "$sim")/../media" "$TB_OFFLINE_DIR/media"; do
    [[ -d "$c" ]] && { media="$(cd "$c" && pwd)"; break; }
done
[[ -n "$media" ]] || warn "미디어 디렉토리가 없습니다 — codec(0)=PCMU 로 떨어질 수 있습니다(녹취 재생 불가)"

# ── -db 인자용 축소 설정 ─────────────────────────────────────
# 운영 csp.json 은 서비스 계정 소유(0600/0660)라 읽히지 않을 수 있다 — 필요한 5키만 따로 만든다.
dbjson="$TB_STATE_DIR/cspsim-db.json"
umask 077
# 비밀번호만 환경변수로 넘긴다 — argv 는 `ps` 로 누구나 보므로 인자에 두지 않는다.
# 사이트 값은 셸 변수라(tb_load_site/tb_site_set 는 export 하지 않는다) 여기서 명시 전달한다.
TB_DB_APP_PASS="$TB_DB_APP_PASS" \
python3 - "$TB_DB_HOST" "$TB_DB_PORT" "$TB_DB_APP_USER" "$TB_DB_NAME" > "$dbjson" <<'PY'
import json, os, sys
h, p, u, d = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
print(json.dumps({"Setup": {"Database": {"Host": h, "Port": p, "User": u,
                                         "Password": os.environ["TB_DB_APP_PASS"],
                                         "DbName": d}}}, indent=1))
PY
chmod 600 "$dbjson"

# ── 시험 1건 실행 + 판정 ─────────────────────────────────────
# run_case <ptt|volte>  → 0 통과 / 1 실패
run_case() {
    local kind="$1"
    local log="$TB_LOG_DIR/70-cspsim-$kind.log"
    local since; since="$(date '+%Y-%m-%dT%H:%M:%S')"
    local kfail=0

    local args=(-server_ip "$TB_SIP_IP" -local_ip "$TB_SIP_IP"
                -count "$TB_CALL_COUNT" -call_duration "$TB_CALL_DURATION"
                -db "$dbjson")
    if [[ "$kind" == ptt ]]; then
        args+=(-mode ptt -scenario group_call -group "$TB_PTT_GROUP" -domain "$TB_PTT_DOMAIN")
    else
        args+=(-mode volte -scenario call -domain "$TB_VOLTE_DOMAIN")
    fi
    [[ -n "$media" ]] && args+=(-media_dir "$media")

    header "── $kind 호시험 ──"
    if [[ "$kind" == ptt ]]; then
        info "cspsim 실행 — mode=ptt group=$TB_PTT_GROUP count=$TB_CALL_COUNT"
    else
        info "cspsim 실행 — mode=volte count=$TB_CALL_COUNT"
    fi
    "$sim" "${args[@]}" 2>&1 | tee "$log" | grep -E "Registered|Call OK|GRANT|codec\(|DB\]" || true

    # cspsim 요약줄은 정렬용 공백이 섞여 나온다 — `  Registered   : 4 / 4  (fail=0)`.
    # 공백 폭에 판정이 걸리지 않게 연속 공백을 하나로 줄인 사본에 대고 찾는다.
    local norm="$TB_STATE_DIR/70-cspsim-$kind.norm"
    tr -s ' \t' ' ' < "$log" > "$norm"

    local N="$TB_CALL_COUNT"
    grep -qE "Registered ?: ?$N / ?$N" "$norm" \
        || { err "[$kind] 등록 실패 — 'Registered : $N / $N' 이 없습니다"; kfail=1; }
    # 'Call OK' 부분일치는 `Call OK/End : 0 / 0` 도 통과시킨다 — 성립 건수를 실제로 본다.
    # VoLTE 는 단말끼리 짝을 지어 걸므로 성립 수가 단말 수의 절반이다 — N/N 을 요구하지 않는다.
    grep -qE "Call OK/End ?: ?[1-9][0-9]* " "$norm" \
        || { err "[$kind] 호 설정 실패 — 'Call OK/End' 성립 건수가 0 입니다"; kfail=1; }

    if [[ "$kind" == ptt ]]; then
        grep -q "GRANT received" "$log" \
            || { err "[$kind] floor 실패 — GRANT received 가 없습니다 (-local_ip / SDP 주소 확인)"; kfail=1; }
    fi

    if grep -q "codec(0)" "$log"; then
        if [[ "$kind" == volte ]]; then
            # 미디어 파일명이 가입 번호와 대응한다. 반입본 media/ 에는 PTT 번호 것만 있어
            # VoLTE 번호는 주입할 파일이 없다 — 설치 결함이 아니라 시험 자료의 범위 문제다.
            warn "[$kind] codec(0)=PCMU — VoLTE 번호용 미디어 파일이 없습니다(반입본은 PTT 번호만). 호 판정과 무관"
        else
            warn "[$kind] codec(0)=PCMU — 미디어 미주입 상태입니다 (녹취 재생 불가). -media_dir 확인"
        fi
    fi

    # 서버측 확인
    if [[ "$kind" == ptt ]]; then
        # 세션 이력의 turn_count — stdout 이 Call OK 여도 floor 가 안 돌면 0 이다.
        # **이번 실행 이후 시작된 세션만** 본다 (옛 이력이 쌓여도 판정이 흔들리지 않게).
        TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" TB_OAM_URL="$TB_OAM_URL" \
            TB_LIB="$_HERE/../lib" TB_SINCE="$since" python3 - <<'PY' || kfail=1
import os, sys
sys.path.insert(0, os.environ['TB_LIB'])
import tb_oam
o = tb_oam.Oam(os.environ['TB_OAM_URL'], os.environ['TB_ADMIN_PASS'])
r = o.req('GET', '/api/v1/ptt/sessions')
items = r if isinstance(r, list) else (r or {}).get('items') or (r or {}).get('sessions') or []
since = os.environ['TB_SINCE']
mine = [s for s in items if (s.get('start_time') or '') >= since]
if not mine:
    print(f"   ⚠ {since} 이후 시작된 PTT 세션이 없습니다 (전체 이력 {len(items)}건)")
    raise SystemExit(1)
bad = 0
for s in mine:
    tc, sc = s.get('turn_count'), s.get('speaker_count')
    # 레코드 키는 session_id(별칭 sesid)다 — 'id' 는 없어서 None 으로 찍혔다.
    sid = s.get('session_id') or s.get('sesid') or s.get('id')
    print(f"   세션 {sid}: turn_count={tc} speaker_count={sc}")
    if not tc:
        bad += 1
if bad:
    print("   ⚠ turn_count=0 인 세션이 있습니다 — floor 실패입니다")
raise SystemExit(1 if bad else 0)
PY
    else
        # VoLTE 는 즉시 조회되는 이력 API 가 없다. 통계는 oam-svc 롤업(StatsRollup.Interval,
        # 기본 60초) 뒤에야 반영되므로 **참고로만** 본다 — 여기서 실패로 치면 주기 때문에
        # 멀쩡한 호가 실패로 찍힌다.
        TB_ADMIN_PASS="$TB_ADMIN_PASS" TB_STATE_DIR="$TB_STATE_DIR" TB_OAM_URL="$TB_OAM_URL" \
            TB_LIB="$_HERE/../lib" python3 - <<'PY' || true
import datetime, os, sys
sys.path.insert(0, os.environ['TB_LIB'])
import tb_oam
o = tb_oam.Oam(os.environ['TB_OAM_URL'], os.environ['TB_ADMIN_PASS'])
day = datetime.date.today().isoformat()
try:
    r = o.req('GET', f'/api/v1/stats/calls?svc=volte&date={day}')
except Exception as e:
    print(f"   (참고) VoLTE 통계 조회 실패: {e}"); raise SystemExit(0)
t = (((r or {}).get('totals') or {}).get('all') or {})
n = t.get('sessions') or 0
if n:
    print(f"   (참고) VoLTE 통계 반영됨 — sessions={n} talked={t.get('talked')} "
          f"duration_sum_sec={t.get('duration_sum_sec')}")
else:
    print("   (참고) VoLTE 통계가 아직 0 입니다 — 롤업 주기(기본 60초) 뒤에 다시 조회하세요")
PY
    fi

    if [[ $kfail -ne 0 ]]; then
        err "[$kind] 판정 실패 — 로그: $log"
        return 1
    fi
    ok "[$kind] 통과"
    return 0
}

# ── 실행 ──────────────────────────────────────────────────────
kinds=()
case "$TB_CALL_KIND" in
    ptt)   kinds=(ptt) ;;
    volte) kinds=(volte) ;;
    both)  kinds=(ptt volte) ;;
esac

fail=0
for k in "${kinds[@]}"; do
    run_case "$k" || fail=1
done

if [[ $fail -ne 0 ]]; then
    die_hint "호시험 판정 실패 — 로그: $TB_LOG_DIR/70-cspsim-*.log" \
        "등록이 실패했다면 접속서비스 도메인 ↔ 저장된 H(A1) 불일치가 첫 용의자입니다 (18·45 단계)." \
        "floor 만 실패했다면 -local_ip 와 CMP 로그의 'Floor from unknown' 을 보세요." \
        "0명 로드로 중단됐다면 -group 이 mcptt_group_id 가 맞는지, 그 kind 의 구독이 있는지 확인하세요."
fi

ok "[70] 통과 — 대상: ${kinds[*]}"
