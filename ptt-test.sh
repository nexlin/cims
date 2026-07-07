#!/usr/bin/env bash
# PTT E2E 테스트 관리 스크립트
#
# 사용법:
#   ./ptt-test.sh start              — 서비스 기동 (CMP → CSP → CSC)
#   ./ptt-test.sh stop               — 서비스 중지
#   ./ptt-test.sh restart            — 재시작
#   ./ptt-test.sh status             — 상태 확인
#   ./ptt-test.sh log                — 실시간 로그 (CMP/CSP/CSC 통합)
#
#   ./ptt-test.sh infra-setup        — 인프라 파일 생성 (access_services.jsonl 등)
#   ./ptt-test.sh db-setup           — DB 테스트 데이터 삽입 (사용자/그룹/멤버)
#   ./ptt-test.sh db-reset           — DB 테스트 데이터 초기화 후 재삽입
#   ./ptt-test.sh db-status          — DB 현재 데이터 조회
#
#   ./ptt-test.sh run prearranged    — prearranged 그룹 콜 시나리오
#   ./ptt-test.sh run broadcast      — broadcast 그룹 콜 시나리오
#   ./ptt-test.sh run chat           — chat 그룹 콜 시나리오
#
#   ./ptt-test.sh stats              — CMP 포트/세션/누수 통계 조회 (A-1-21 검증)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$SCRIPT_DIR/build/dist"
SVC="bash $DIST_DIR/agent/bin/cims-svc"

# ── 테스트 설정 ───────────────────────────────────────────────
SERVER_IP="121.134.202.23"
SIP_PORT="5160"          # 기본 5060 + 오프셋 100
CSC_IP="121.134.202.23"
CSC_PORT="4530"          # 기본 4430 + 오프셋 100

# DB 접속 (build/dist/csp/config/csp.json 기준)
DB_HOST="127.0.0.1"
DB_PORT="3306"
DB_USER="cims"
DB_PASS="cims1234"
DB_NAME="cims"
MYSQL="mysql -h$DB_HOST -P$DB_PORT -u$DB_USER -p$DB_PASS $DB_NAME"

# 테스트 사용자 (PTT MSISDN)
USERS=(1001 1002 1003 1004)
USER_PWD="1234"
SIP_DOMAIN="csp"
IMSI_BASE="001011000000001"   # USERS[0]의 IMSI; 이후 세션은 +1씩 자동 증가

# 테스트 그룹
GROUP_PREARRANGED="g001"
GROUP_BROADCAST="g002"
GROUP_CHAT="g003"

# CMP 제어 포트 (기본 9000 + 오프셋 100)
CMP_CONTROL_PORT="9100"

# cspsim 옵션
CSPSIM="$SCRIPT_DIR/build/bin/cspsim"
CALL_DURATION="15"

# ── 서비스 관리 ───────────────────────────────────────────────

_check_build() {
    if [[ ! -f "$DIST_DIR/cmp/bin/cmp" ]]; then
        echo "[ERROR] 빌드 결과물이 없습니다. 먼저 빌드를 실행하세요: ./cims.sh build"
        exit 1
    fi
}

cmd_start() {
    _check_build
    echo "=== PTT 테스트 서비스 기동 ==="
    echo ""
    echo "[1/5] CMP 기동..."
    $SVC start cmp
    echo ""
    echo "[2/5] CSP 기동..."
    $SVC start csp
    echo ""
    echo "[3/5] CSC 기동..."
    $SVC start csc
    echo ""
    echo "[4/5] OAM 기동... (콘솔 UI 백엔드)"
    $SVC start oam
    echo ""
    echo "[5/5] Console 기동... (UI, http://${SERVER_IP}:3001)"
    $SVC start console
    echo ""
    echo "=== 상태 확인 ==="
    $SVC status
}

cmd_stop() {
    echo "=== PTT 테스트 서비스 중지 ==="
    $SVC stop console || true
    $SVC stop oam || true
    $SVC stop csc || true
    $SVC stop csp || true
    $SVC stop cmp || true
    echo ""
    echo "=== 상태 확인 ==="
    $SVC status
}

cmd_restart() {
    cmd_stop
    echo ""
    echo "[대기] 포트 해제 대기 중 (3초)..."
    sleep 3
    cmd_start
}

cmd_status() {
    $SVC status
}

_csp_log_file() {
    # CSP는 내부 롤링 파일(csp/log/csp_YYYYMMDD_N.log)에 실제 로그를 씀
    # stdout redirect(log/csp.log)는 거의 비어있으므로 롤링 파일을 우선 사용
    local rolling
    rolling=$(ls -t "$DIST_DIR/csp/log/csp_"*.log 2>/dev/null | head -1)
    echo "${rolling:-$DIST_DIR/log/csp.log}"
}

cmd_log() {
    local target="${1:-all}"
    case "$target" in
        csp) tail -f "$(_csp_log_file)" 2>/dev/null ;;
        cmp) tail -f "$DIST_DIR/log/cmp.log" 2>/dev/null ;;
        csc) tail -f "$DIST_DIR/log/csc.log" 2>/dev/null ;;
        oam) tail -f "$DIST_DIR/log/oam.log" 2>/dev/null ;;
        all)
            echo "=== CSP + CMP 실시간 로그 — [CSP] / [CMP] 접두어로 구분 (Ctrl+C 종료) ==="
            (tail -f "$(_csp_log_file)" 2>/dev/null | sed 's/^/[CSP] /' &
             tail -f "$DIST_DIR/log/cmp.log" 2>/dev/null | sed 's/^/[CMP] /' &
             wait)
            ;;
        *)
            echo "사용법: $0 log [csp|cmp|csc|oam|all]"
            ;;
    esac
}

# ── 인프라 설정 ───────────────────────────────────────────────

cmd_infra_setup() {
    echo "=== 인프라 파일 설정 ==="

    # CSP가 jsonl 파일을 읽는 공용 디렉토리: build/dist/config/
    local cfg_dir="$DIST_DIR/config"
    mkdir -p "$cfg_dir"

    # local_nodes.jsonl — CSP SIP 수신 포트 (primary UDP)
    local node_file="$cfg_dir/local_nodes.jsonl"
    if [[ -f "$node_file" ]]; then
        echo "[SKIP] local_nodes.jsonl 이미 존재: $node_file"
    else
        python3 -c "
import json, uuid
entry = {
    'id': str(uuid.uuid4()), 'name': 'access-udp', 'enabled': True,
    'is_primary': True, 'edge': 'access', 'bind_ip': '0.0.0.0',
    'bind_port': $SIP_PORT, 'protocol': 'UDP', 'tags': [], 'note': 'port-offset +100'
}
print(json.dumps(entry))
" > "$node_file"
        echo "[OK] local_nodes.jsonl 생성: $node_file (port=$SIP_PORT)"
    fi

    # CSP Xcap.Host 플레이스홀더 교정 (@CSC_IP@ → 실제 CSC IP)
    local csp_cfg="$DIST_DIR/csp/config/csp.json"
    python3 -c "
import json
with open('$csp_cfg') as f: c = json.load(f)
xcap = c.get('Setup', {}).get('Xcap', {})
if xcap.get('Host', '').startswith('@'):
    xcap['Host'] = '$CSC_IP'
    c['Setup']['Xcap'] = xcap
    with open('$csp_cfg', 'w') as f: json.dump(c, f, indent=2, ensure_ascii=False)
    print('[OK] csp.json Xcap.Host 교정: $CSC_IP')
else:
    print('[SKIP] csp.json Xcap.Host 이미 설정됨:', xcap.get('Host'))
"

    # OAM 설정 — CimsRuntimeDir 경로 + DB 패스워드 + ServiceLogging.Dir
    local oam_cfg="$DIST_DIR/oam/config/oam.json"
    local runtime_dir="$DIST_DIR/ext_mnt/runtime"
    local svc_log_dir="$DIST_DIR/ext_mnt/service_log"
    mkdir -p "$runtime_dir" "$svc_log_dir"
    local oam_changed
    oam_changed=$(python3 -c "
import json
with open('$oam_cfg') as f: c = json.load(f)
changed = False
if c.get('CimsRuntimeDir','').startswith('/home/cims'):
    c['CimsRuntimeDir'] = '$runtime_dir'
    changed = True
    print('[OK] oam.json CimsRuntimeDir 경로 수정')
else:
    print('[SKIP] oam.json CimsRuntimeDir 이미 수정됨')
db = c.get('CimsDatabase', {})
if db.get('Password') != '$DB_PASS':
    db['Password'] = '$DB_PASS'
    c['CimsDatabase'] = db
    changed = True
    print('[OK] oam.json CimsDatabase.Password 수정')
else:
    print('[SKIP] oam.json CimsDatabase.Password 이미 올바름')
cur_svc = c.get('ServiceLogging', {}).get('Dir', '')
if cur_svc != '$svc_log_dir':
    c.setdefault('ServiceLogging', {})['Dir'] = '$svc_log_dir'
    changed = True
    print('[OK] oam.json ServiceLogging.Dir 교정:', cur_svc, '->', '$svc_log_dir')
else:
    print('[SKIP] oam.json ServiceLogging.Dir 이미 올바름')
if changed:
    with open('$oam_cfg','w') as f: json.dump(c, f, indent=4, ensure_ascii=False)
    print('__changed__')
")
    echo "$oam_changed" | grep -v "__changed__"
    if echo "$oam_changed" | grep -q "__changed__"; then
        # 설정이 바뀌었고 OAM이 실행 중이면 재시작
        local oam_pid="$DIST_DIR/run/oam.pid"
        if [[ -f "$oam_pid" ]] && kill -0 "$(cat "$oam_pid")" 2>/dev/null; then
            echo "[INFO] OAM 재시작 (설정 반영)..."
            $SVC restart oam
        fi
    fi

    # access_services.jsonl — CSP가 PTT 도메인/realm을 인식하기 위해 필요 (항상 덮어씀)
    local svc_file="$cfg_dir/access_services.jsonl"
    cat > "$svc_file" <<EOF
{"id":1,"name":"ptt","kind":"ptt","domain":"$SIP_DOMAIN","auth_realm":"$SIP_DOMAIN","allowed_local_node_refs":["access-udp"]}
EOF
    echo "[OK] access_services.jsonl 생성/갱신: $svc_file"

}

# ── DB 관리 ───────────────────────────────────────────────────

_db_check() {
    if ! command -v mysql &>/dev/null; then
        echo "[ERROR] mysql 클라이언트가 없습니다."
        exit 1
    fi
    if ! $MYSQL -e "SELECT 1" &>/dev/null; then
        echo "[ERROR] DB 연결 실패 ($DB_USER@$DB_HOST:$DB_PORT/$DB_NAME)"
        exit 1
    fi
}

cmd_db_setup() {
    echo "=== DB 테스트 데이터 삽입 ==="
    _db_check

    $MYSQL <<SQL
-- ── 테스트 사용자 삽입 ──────────────────────────────────────
-- login_id는 UNIQUE 제약 — PTT 전용이라 콘솔 로그인 불필요, ptt_MSISDN 형태로 구분
INSERT IGNORE INTO users (name, login_id, password, role, email, org_id, create_time, update_time)
VALUES
  ('PTT Test 1', 'ptt1001', '', 'user', '', '', NOW(), NOW()),
  ('PTT Test 2', 'ptt1002', '', 'user', '', '', NOW(), NOW()),
  ('PTT Test 3', 'ptt1003', '', 'user', '', '', NOW(), NOW()),
  ('PTT Test 4', 'ptt1004', '', 'user', '', '', NOW(), NOW());

-- users.id를 가져와서 ptt_subscriptions에 삽입
SET @u1 = (SELECT id FROM users WHERE login_id='ptt1001' LIMIT 1);
SET @u2 = (SELECT id FROM users WHERE login_id='ptt1002' LIMIT 1);
SET @u3 = (SELECT id FROM users WHERE login_id='ptt1003' LIMIT 1);
SET @u4 = (SELECT id FROM users WHERE login_id='ptt1004' LIMIT 1);

INSERT IGNORE INTO ptt_subscriptions (id, user_id, passwd, dnd, forward_id, service_ref, imsi)
VALUES
  ('1001', @u1, '$USER_PWD', 0, '', 'ptt', '001011000000001'),
  ('1002', @u2, '$USER_PWD', 0, '', 'ptt', '001011000000002'),
  ('1003', @u3, '$USER_PWD', 0, '', 'ptt', '001011000000003'),
  ('1004', @u4, '$USER_PWD', 0, '', 'ptt', '001011000000004');

-- ── 테스트 그룹 삽입 ────────────────────────────────────────
INSERT IGNORE INTO ptt_groups (mcptt_group_id, name, group_type, require_affiliation, created_at)
VALUES
  ('$GROUP_PREARRANGED', 'PTT Test - Prearranged', 'prearranged', 1, NOW()),
  ('$GROUP_BROADCAST',   'PTT Test - Broadcast',   'broadcast',   1, NOW()),
  ('$GROUP_CHAT',        'PTT Test - Chat',         'chat',        1, NOW());

-- ── 그룹 멤버 매핑 ──────────────────────────────────────────
SET @g1 = (SELECT id FROM ptt_groups WHERE mcptt_group_id='$GROUP_PREARRANGED');
SET @g2 = (SELECT id FROM ptt_groups WHERE mcptt_group_id='$GROUP_BROADCAST');
SET @g3 = (SELECT id FROM ptt_groups WHERE mcptt_group_id='$GROUP_CHAT');

-- prearranged: 4명 모두, 1001이 chair
INSERT IGNORE INTO ptt_group_members (group_id, user_id, priority, role) VALUES
  (@g1, '1001', 1, 'chair'),
  (@g1, '1002', 2, 'participant'),
  (@g1, '1003', 3, 'participant'),
  (@g1, '1004', 4, 'participant');

-- broadcast: 4명 모두, 1001이 broadcast 개시자(chair)
INSERT IGNORE INTO ptt_group_members (group_id, user_id, priority, role) VALUES
  (@g2, '1001', 1, 'chair'),
  (@g2, '1002', 2, 'participant'),
  (@g2, '1003', 3, 'participant'),
  (@g2, '1004', 4, 'participant');

-- chat: 4명 모두, 동등 participant
INSERT IGNORE INTO ptt_group_members (group_id, user_id, priority, role) VALUES
  (@g3, '1001', 1, 'participant'),
  (@g3, '1002', 2, 'participant'),
  (@g3, '1003', 3, 'participant'),
  (@g3, '1004', 4, 'participant');

SELECT 'DB 설정 완료' AS result;
SQL

    echo ""
    cmd_db_status
}

cmd_db_reset() {
    echo "=== DB 테스트 데이터 초기화 ==="
    _db_check

    $MYSQL <<SQL
-- 테스트 데이터만 삭제 (다른 데이터 보호)
DELETE FROM ptt_group_members
  WHERE group_id IN (
    SELECT id FROM ptt_groups
    WHERE mcptt_group_id IN ('$GROUP_PREARRANGED','$GROUP_BROADCAST','$GROUP_CHAT')
  );
DELETE FROM ptt_affiliations
  WHERE group_id IN (
    SELECT id FROM ptt_groups
    WHERE mcptt_group_id IN ('$GROUP_PREARRANGED','$GROUP_BROADCAST','$GROUP_CHAT')
  );
DELETE FROM ptt_groups
  WHERE mcptt_group_id IN ('$GROUP_PREARRANGED','$GROUP_BROADCAST','$GROUP_CHAT');
DELETE FROM ptt_subscriptions
  WHERE id IN ('1001','1002','1003','1004');
DELETE FROM users
  WHERE name IN ('PTT Test 1','PTT Test 2','PTT Test 3','PTT Test 4');

SELECT 'DB 초기화 완료' AS result;
SQL

    echo ""
    echo "재삽입 중..."
    cmd_db_setup
}

cmd_db_status() {
    echo "=== DB 현재 상태 ==="
    _db_check

    echo ""
    echo "── PTT 가입자 ──"
    $MYSQL -e "SELECT id, user_id, dnd FROM ptt_subscriptions WHERE id IN ('1001','1002','1003','1004');"

    echo ""
    echo "── PTT 그룹 ──"
    $MYSQL -e "SELECT id, mcptt_group_id, name, group_type FROM ptt_groups WHERE mcptt_group_id IN ('$GROUP_PREARRANGED','$GROUP_BROADCAST','$GROUP_CHAT');"

    echo ""
    echo "── 그룹 멤버 ──"
    $MYSQL -e "
SELECT g.mcptt_group_id, m.user_id, m.role, m.priority
FROM ptt_group_members m
JOIN ptt_groups g ON g.id = m.group_id
WHERE g.mcptt_group_id IN ('$GROUP_PREARRANGED','$GROUP_BROADCAST','$GROUP_CHAT')
ORDER BY g.mcptt_group_id, m.priority;"
}

# ── CMP STATS 조회 ────────────────────────────────────────────

cmd_stats() {
    if ! command -v nc &>/dev/null; then
        echo "[ERROR] nc(netcat)가 없습니다. sudo apt-get install netcat-openbsd"
        exit 1
    fi

    echo "=== CMP STATS ($SERVER_IP:$CMP_CONTROL_PORT) ==="
    local resp
    resp=$(echo '{"payload":{"cmd":"STATS_REQUEST"},"trans_id":1}' \
        | nc -u -w2 "$SERVER_IP" "$CMP_CONTROL_PORT" 2>/dev/null)

    if [[ -z "$resp" ]]; then
        echo "[ERROR] 응답 없음 — CMP가 실행 중인지 확인하세요."
        exit 1
    fi

    echo "$resp" | python3 -c "
import json, sys
d = json.load(sys.stdin)
p = d.get('response', d.get('payload', d))

fields = [
    ('sessions',              'RTP 세션 수'),
    ('groups',                'PTT 그룹 수'),
    ('rtp_ports_total',       'VoIP 포트 전체'),
    ('rtp_ports_used',        'VoIP 포트 사용 중'),
    ('rtp_ports_free',        'VoIP 포트 여유'),
    ('ptt_rtp_ports_total',   'PTT RTP 포트 전체'),
    ('ptt_rtp_ports_used',    'PTT RTP 포트 사용 중'),
    ('ptt_rtp_ports_free',    'PTT RTP 포트 여유'),
    ('ptt_floor_ports_total', 'PTT Floor 포트 전체'),
    ('ptt_floor_ports_used',  'PTT Floor 포트 사용 중'),
    ('ptt_floor_ports_free',  'PTT Floor 포트 여유'),
    ('session_timeout',       'Session Timeout (s)'),
    ('orphan_reclaim_sec',    'Orphan Reclaim (s)'),
    ('leak_reclaim_total',    '누수 회수 누계'),
]
for key, label in fields:
    if key in p:
        val = p[key]
        flag = ' ← 누수 의심' if key == 'leak_reclaim_total' and val > 0 else ''
        print(f'  {label:<26} {val}{flag}')
"
}

# ── 시나리오 실행 ─────────────────────────────────────────────

_clean_affiliations() {
    if ! $MYSQL -e "SELECT 1" &>/dev/null 2>&1; then
        echo "[WARN] DB 연결 실패 — affiliate 초기화 생략"
        return
    fi
    local ids
    ids=$(printf "'%s'," "${USERS[@]}")
    ids="${ids%,}"
    $MYSQL -e "DELETE FROM ptt_affiliations WHERE user_id IN ($ids);" 2>/dev/null
    echo "[OK] ptt_affiliations 초기화 (${USERS[*]})"
}

_run_cspsim() {
    local group_id="$1"
    local label="$2"
    local count="${3:-${#USERS[@]}}"   # 기본값: 전체 사용자 수

    if [[ ! -f "$CSPSIM" ]]; then
        echo "[ERROR] cspsim 바이너리가 없습니다: $CSPSIM"
        exit 1
    fi

    echo "=== PTT 시나리오: $label (그룹: $group_id, 사용자: ${count}명) ==="
    echo "서버: $SERVER_IP:$SIP_PORT | CSC: $CSC_IP:$CSC_PORT"
    echo "도메인: $SIP_DOMAIN"
    echo ""

    "$CSPSIM" \
        -server_ip  "$SERVER_IP" \
        -server_port "$SIP_PORT" \
        -count      "$count" \
        -user       "${USERS[0]}" \
        -auth_id    "${IMSI_BASE}@${SIP_DOMAIN}" \
        -domain     "$SIP_DOMAIN" \
        -password   "$USER_PWD" \
        -mode       ptt \
        -group      "$group_id" \
        -scenario   group_call \
        -csc_ip     "$CSC_IP" \
        -csc_port   "$CSC_PORT" \
        -csc_tls \
        -call_duration "$CALL_DURATION"
}

cmd_run() {
    local scenario="${1:-}"
    case "$scenario" in
        # ── A 시나리오 ──────────────────────────────────────────
        prearranged) _clean_affiliations; _run_cspsim "$GROUP_PREARRANGED" "A-1: Prearranged" ;;
        broadcast)   _clean_affiliations; _run_cspsim "$GROUP_BROADCAST"   "A-1: Broadcast" ;;
        chat)        _clean_affiliations; _run_cspsim "$GROUP_CHAT"         "A-1: Chat" ;;
        # ── B 시나리오 ──────────────────────────────────────────
        b1)
            _clean_affiliations
            echo "[B-1] 1004 제외, 1001·1002·1003만 REGISTER/affiliate"
            _run_cspsim "$GROUP_PREARRANGED" "B-1: 부분 등록 (3명)" 3
            ;;
        b3)
            _clean_affiliations
            echo "[B-3] 1001만 REGISTER/affiliate (단독 세션)"
            _run_cspsim "$GROUP_PREARRANGED" "B-3: 개시자 단독" 1
            ;;
        *)
            echo "사용법: $0 run <시나리오>"
            echo ""
            echo "  A 시나리오 (정상 흐름):"
            echo "    prearranged    4명 prearranged 그룹콜"
            echo "    broadcast      4명 broadcast 그룹콜"
            echo "    chat           4명 chat 그룹콜"
            echo ""
            echo "  B 시나리오 (부분 등록):"
            echo "    b1             B-1: 1004 REGISTER 없음 (3명만)"
            echo "    b3             B-3: 개시자(1001) 단독"
            exit 1
            ;;
    esac
}

# ── 진입점 ───────────────────────────────────────────────────

case "${1:-}" in
    start)        cmd_start ;;
    stop)         cmd_stop ;;
    restart)      cmd_restart ;;
    status)       cmd_status ;;
    log)          cmd_log "${2:-}" ;;
    infra-setup)  cmd_infra_setup ;;
    db-setup)     cmd_db_setup ;;
    db-reset)     cmd_db_reset ;;
    db-status)    cmd_db_status ;;
    run)          cmd_run "${2:-}" ;;
    stats)        cmd_stats ;;
    *)
        echo "사용법: $0 {start|stop|restart|status|log|infra-setup|db-setup|db-reset|db-status|run|stats}"
        echo ""
        echo "  start              서비스 기동 (CMP → CSP → CSC)"
        echo "  stop               서비스 중지"
        echo "  restart            재시작"
        echo "  status             상태 확인"
        echo "  log                실시간 로그"
        echo ""
        echo "  infra-setup        access_services.jsonl 등 인프라 파일 생성"
        echo "  db-setup           DB 테스트 데이터 삽입"
        echo "  db-reset           DB 테스트 데이터 초기화 후 재삽입"
        echo "  db-status          DB 현재 데이터 조회"
        echo ""
        echo "  run prearranged    A-1: 4명 prearranged 그룹콜"
        echo "  run broadcast      A-1: 4명 broadcast 그룹콜"
        echo "  run chat           A-1: 4명 chat 그룹콜"
        echo "  run b1             B-1: 1004 REGISTER 없음 (3명)"
        echo "  run b3             B-3: 개시자(1001) 단독"
        echo ""
        echo "  stats              CMP 포트/세션/누수 통계 조회"
        exit 1
        ;;
esac
