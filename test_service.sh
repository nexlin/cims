#!/usr/bin/env bash
# =============================================================
# CIMS 서비스 검증 시험 스크립트
# 수동으로 단계별 실행: 각 단계 복사하여 터미널에서 실행
# =============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST="$SCRIPT_DIR/build/dist"
SIM="$DIST/cspsim/bin/cspsim"
DB="-db $DIST/csp/config/csp.json"
MEDIA="-media_dir $DIST/cspsim/media"
SERVER="192.168.0.2"

# ─────────────────────────────────────────────
# STEP 0: 빌드 (소스 변경 시)
# ─────────────────────────────────────────────
# cd /home/nex/work/cims/build && make -j$(nproc) && make dist
# 또는 수동 바이너리 복사:
# cp build/bin/cmp build/dist/cmp/bin/cmp
# cp build/bin/csp build/dist/csp/bin/csp
# cp build/bin/cspsim build/dist/cspsim/bin/cspsim
# cp csc/bin/csc_pihttp/src/csc_flow.py build/dist/csc/src/csc_flow.py
# cp csc/bin/csc_pihttp/src/cims_recording.py build/dist/csc/src/cims_recording.py

# ─────────────────────────────────────────────
# STEP 1: 전체 중단
# ─────────────────────────────────────────────
step1_stop() {
    echo "=== STEP 1: 전체 중단 ==="
    cd "$SCRIPT_DIR"
    ./cims.sh stop all
}

# ─────────────────────────────────────────────
# STEP 2: 로그 데이터 정리
# ─────────────────────────────────────────────
step2_clean() {
    echo "=== STEP 2: 로그 정리 ==="
    rm -rf "$DIST/ext_mnt/service_log" "$DIST/ext_mnt/msg_log"
    mkdir -p "$DIST/ext_mnt/service_log" "$DIST/ext_mnt/msg_log"
    rm -f "$DIST/log/"*.log
    echo "done"
}

# ─────────────────────────────────────────────
# STEP 3: 전체 기동
# ─────────────────────────────────────────────
step3_start() {
    echo "=== STEP 3: 전체 기동 ==="
    cd "$SCRIPT_DIR"
    ./cims.sh start all
}

# ─────────────────────────────────────────────
# STEP 4: VoLTE 음성통화 #1 (60초)
# ─────────────────────────────────────────────
step4_voip_voice_1() {
    echo "=== STEP 4: VoLTE 음성 #1 (60s) ==="
    cd "$DIST/cspsim"
    $SIM -server_ip $SERVER -mode volte -scenario call -count 2 -call_duration 60 \
        $MEDIA -no_video $DB
}

# ─────────────────────────────────────────────
# STEP 5: VoLTE 음성통화 #2 (60초)
# ─────────────────────────────────────────────
step5_voip_voice_2() {
    echo "=== STEP 5: VoLTE 음성 #2 (60s) ==="
    cd "$DIST/cspsim"
    $SIM -server_ip $SERVER -mode volte -scenario call -count 2 -call_duration 60 \
        $MEDIA -no_video $DB
}

# ─────────────────────────────────────────────
# STEP 6: VoLTE 영상통화 #1 (60초)
# ─────────────────────────────────────────────
step6_voip_video_1() {
    echo "=== STEP 6: VoLTE 영상 #1 (60s) ==="
    cd "$DIST/cspsim"
    $SIM -server_ip $SERVER -mode volte -scenario call -count 2 -call_duration 60 \
        $MEDIA $DB
}

# ─────────────────────────────────────────────
# STEP 7: VoLTE 영상통화 #2 (60초)
# ─────────────────────────────────────────────
step7_voip_video_2() {
    echo "=== STEP 7: VoLTE 영상 #2 (60s) ==="
    cd "$DIST/cspsim"
    $SIM -server_ip $SERVER -mode volte -scenario call -count 2 -call_duration 60 \
        $MEDIA $DB
}

# ─────────────────────────────────────────────
# STEP 8: PTT 그룹 #1 (5명, 15초×5 발언)
#   그룹: +82571910001 (video=1, 멤버 5명)
#   도메인: ptt.mnc033.mcc450.3gppnetwork.org
# ─────────────────────────────────────────────
step8_ptt_group_1() {
    echo "=== STEP 8: PTT 그룹 #1 (+82571910001, 15s×5) ==="
    cd "$DIST/cspsim"
    $SIM -server_ip $SERVER -mode ptt -scenario group_call \
        -group +82571910001 -count 5 -call_duration 15 \
        $MEDIA $DB
}

# ─────────────────────────────────────────────
# STEP 9: PTT 그룹 #2 (5명, 15초×5 발언)
#   그룹: +82571910002 (video=0, 멤버 5명)
#   도메인: ptt.mnc033.mcc450.3gppnetwork.org
# ─────────────────────────────────────────────
step9_ptt_group_2() {
    echo "=== STEP 9: PTT 그룹 #2 (+82571910002, 15s×5) ==="
    cd "$DIST/cspsim"
    $SIM -server_ip $SERVER -mode ptt -scenario group_call \
        -group +82571910002 -count 5 -call_duration 15 \
        $MEDIA $DB
}

# ─────────────────────────────────────────────
# STEP 10: 결과 확인
# ─────────────────────────────────────────────
step10_verify() {
    echo "=== STEP 10: 검증 결과 ==="
    SVC="$DIST/ext_mnt/service_log"
    MSG="$DIST/ext_mnt/msg_log"

    echo ""
    echo "--- VoIP 세션 ---"
    find "$SVC/voip" -name "*.d" -type d 2>/dev/null | while read d; do
        ct=$(python3 -c "import json; print(json.load(open('$d/call.json')).get('call_type','?'))" 2>/dev/null)
        dur=$(python3 -c "import json; print(json.load(open('$d/call.json')).get('duration',0))" 2>/dev/null)
        seg=$(wc -l < "$d/segments.jsonl" 2>/dev/null || echo 0)
        echo "  $ct  dur=${dur}s  segs=$seg"
    done | sort

    echo ""
    echo "--- PTT 세션 ---"
    find "$SVC/ptt" -name "*.d" -type d 2>/dev/null | while read d; do
        gid=$(echo "$d" | grep -oP 'ptt/\K[^/]+')
        seg=$(wc -l < "$d/segments.jsonl" 2>/dev/null || echo 0)
        echo "  그룹=$gid  segs=$seg"
    done

    echo ""
    echo "--- Flow 로그 ---"
    find "$SVC" -name "*.flow.jsonl" -exec sh -c 'echo "  $(wc -l < "$1") lines  $(basename $1)"' _ {} \; | sort -rn

    echo ""
    echo "--- Body 로그 ---"
    find "$MSG" -name "*.jsonl" -exec sh -c 'echo "  $(wc -l < "$1") lines  $(basename $1)"' _ {} \; | sort -rn

    echo ""
    echo "--- API ---"
    echo -n "녹취: "; curl -sk "https://localhost:4421/api/v1/recordings" 2>&1 | python3 -c "import json,sys; d=json.load(sys.stdin); print(f'{d[\"total\"]}건')"
    TODAY=$(date +%Y-%m-%d)
    echo -n "Flow: "; curl -sk "https://localhost:4421/api/v1/flow/list?date=$TODAY" 2>&1 | python3 -c "import json,sys; ids=json.load(sys.stdin).get('call_ids',[]); print(f'{len(ids)}건')"
}

# ─────────────────────────────────────────────
# 사용법
# ─────────────────────────────────────────────
usage() {
    cat <<EOF
CIMS 서비스 검증 시험

사용법: $0 <step>

  step1   전체 중단
  step2   로그 정리
  step3   전체 기동
  step4   VoLTE 음성 #1 (60s)
  step5   VoLTE 음성 #2 (60s)
  step6   VoLTE 영상 #1 (60s)
  step7   VoLTE 영상 #2 (60s)
  step8   PTT 그룹 #1 (15s×5)
  step9   PTT 그룹 #2 (15s×5)
  step10  결과 확인
  all     step1~step10 순차 실행

예시:
  $0 step1          # 전체 중단
  $0 step3          # 전체 기동
  $0 step4          # 음성통화 #1 실행
  $0 step10         # 결과 확인
  $0 all            # 전체 순차 실행
EOF
}

case "${1:-}" in
    step1)  step1_stop ;;
    step2)  step2_clean ;;
    step3)  step3_start ;;
    step4)  step4_voip_voice_1 ;;
    step5)  step5_voip_voice_2 ;;
    step6)  step6_voip_video_1 ;;
    step7)  step7_voip_video_2 ;;
    step8)  step8_ptt_group_1 ;;
    step9)  step9_ptt_group_2 ;;
    step10) step10_verify ;;
    all)
        step1_stop
        step2_clean
        step3_start
        sleep 2
        step4_voip_voice_1
        step5_voip_voice_2
        step6_voip_video_1
        step7_voip_video_2
        step8_ptt_group_1
        step9_ptt_group_2
        step10_verify
        ;;
    *) usage ;;
esac
