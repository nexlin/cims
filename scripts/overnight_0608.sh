#!/bin/bash
# 오버나잇 안정성 시험 — VoLTE(1cps/ht10) + PTT(g001 40명 10s floor 무한순환).
#   deadline = 2026-06-08 07:00:00 (사용자 지정). 각 부하는 crash/exit 시 자동 재시작.
#   180초 주기 health 샘플러가 report.jsonl 에 기록. 중단: touch <DIR>/overnight_0608.stop
#   ⚠️로그는 디스크(/home/cims, xfs)에 기록 — 구버전이 /tmp(tmpfs+usrquota)에 RTP STATS 폭증
#     기록 → tmpfs RAM 잠식+쿼터초과(EDQUOT)로 셸 마비된 사고 재발 방지.
#   ⚠️cspsim 출력은 'RTP STATS' 등 고빈도 노이즈를 grep 필터링 후 기록(로그 폭증 차단).
set -u
CIMS=/home/cims/work/cims
SIM="$CIMS/build/bin/cspsim"
DB=/opt/cims-agent/csp/config/csp.json
SRV=121.161.164.47
DIR=/home/cims/overnight_0608           # ← 디스크(xfs). tmpfs(/tmp) 금지.
STOP=$DIR/overnight_0608.stop
mkdir -p "$DIR"
rm -f "$STOP"
DEADLINE=$(date -d '2026-06-08 07:00:00' +%s)
REPORT="$DIR/report.jsonl"
VLOG="$DIR/volte.log"
PLOG="$DIR/ptt.log"
# 고빈도 노이즈 필터(로그 폭증 차단): RTP STATS / per-packet 류 제거, 콜·floor 이벤트만 보존.
NOISE='RTP STATS|\[RTP|RTP recv|RTP send'
echo "$$" > "$DIR/orchestrator.pid"

log(){ echo "[$(date '+%F %H:%M:%S')] $*" >> "$DIR/orchestrator.log"; }
log "START deadline=$(date -d @$DEADLINE '+%F %T') (VoLTE 1cps/ht10 + PTT g001 40명/floor10s) DIR=$DIR"

# ── VoLTE 부하: 1cps, ht10, 음성. crash 시 재시작. 노이즈 필터 후 기록 ──
volte_loop(){
  local c=0
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    c=$((c+1)); log "VoLTE launch #$c"
    "$SIM" -server_ip "$SRV" -server_port 5060 -mode volte -scenario call \
      -domain ims.mnc033.mcc450.3gppnetwork.org -cps 1 -ht 10 -calls 1000000 \
      -db "$DB" -count 60 -db_offset 0 -no_video -media_dir "$CIMS/tests/media" 2>&1 \
      | grep --line-buffered -avE "$NOISE" >> "$VLOG"
    log "VoLTE exited #$c — restart in 5s"
    [ -f "$STOP" ] && break; sleep 5
  done
}

# ── PTT 부하: g001 40명, floor 10s 무한순환. crash 시 재시작. 노이즈 필터 후 기록 ──
ptt_loop(){
  local c=0
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    c=$((c+1)); log "PTT launch #$c"
    "$SIM" -server_ip "$SRV" -server_port 5060 -mode ptt -group g001 -scenario group-call \
      -domain ptt.mnc033.mcc450.3gppnetwork.org -count 40 -floor_hold 10 -floor_loop \
      -db "$DB" -no_video -media_dir "$CIMS/tests/media" 2>&1 \
      | grep --line-buffered -avE "$NOISE" >> "$PLOG"
    log "PTT exited #$c — restart in 5s"
    [ -f "$STOP" ] && break; sleep 5
  done
}

# ── health 샘플러: 180초 주기 report.jsonl + 로그 크기 안전 cap(>200MB 절단) ──
sampler(){
  local TOK; TOK=$(cat /tmp/oam_tok.txt 2>/dev/null)
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    # 안전장치: 로그가 비정상적으로 커지면 절단(폭증 방어 2차)
    find "$DIR" -maxdepth 1 -name '*.log' -size +200M -exec truncate -s 0 {} \; 2>/dev/null
    local now v_alive p_alive v_end v_200 v_fail v_reg p_join p_gnf leak deploys disk
    now=$(date '+%F %T')
    pgrep -x cspsim >/dev/null && { pgrep -af 'mode volte'>/dev/null && v_alive=1||v_alive=0; pgrep -af 'mode ptt'>/dev/null && p_alive=1||p_alive=0; } || { v_alive=0; p_alive=0; }
    v_end=$(grep -ac 'CALL ENDED' "$VLOG" 2>/dev/null); v_end=${v_end:-0}
    v_200=$(grep -a 'CALL ENDED' "$VLOG" 2>/dev/null | grep -ac 'status=200'); v_200=${v_200:-0}
    v_fail=$(grep -a 'CALL ENDED' "$VLOG" 2>/dev/null | grep -avc 'status=200'); v_fail=${v_fail:-0}
    v_reg=$(grep -ac 'REGISTER FAILED' "$VLOG" 2>/dev/null); v_reg=${v_reg:-0}
    p_join=$(grep -ac 'JOIN\|AFFILIATE' "$PLOG" 2>/dev/null); p_join=${p_join:-0}
    p_gnf=$(grep -aic 'group not found\|Group Not Found' "$PLOG" 2>/dev/null); p_gnf=${p_gnf:-0}
    disk=$(df -P "$DIR" | awk 'NR==2{print $5}')
    leak=$(curl -sk -m 5 -H "Authorization: Bearer $TOK" "https://127.0.0.1:4419/api/v1/stats/leak-reclaims" 2>/dev/null | head -c 300)
    deploys=$(curl -sk -m 5 -H "Authorization: Bearer $TOK" "https://127.0.0.1:4419/api/v1/deployments" 2>/dev/null \
      | python3 -c "import sys,json;d=json.load(sys.stdin);d=d if isinstance(d,list) else d.get('items',[]);print(','.join(f\"{x['process_name']}:{x.get('status')}\" for x in d if x['process_name'] in ('csp','cmp','csc')))" 2>/dev/null)
    printf '{"ts":"%s","volte_alive":%s,"ptt_alive":%s,"v_end":%s,"v_200":%s,"v_fail":%s,"v_reg_fail":%s,"p_join":%s,"p_gnf":%s,"disk":"%s","deploys":"%s","leak":%s}\n' \
      "$now" "$v_alive" "$p_alive" "$v_end" "$v_200" "$v_fail" "$v_reg" "$p_join" "$p_gnf" "$disk" "$deploys" "${leak:-null}" >> "$REPORT"
    sleep 180
  done
}

volte_loop & VPID=$!
ptt_loop   & PPID=$!
sampler    & SPID=$!
log "spawned volte=$VPID ptt=$PPID sampler=$SPID"

while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do sleep 30; done
log "deadline/stop reached — terminating load"
touch "$STOP"
pkill -x cspsim 2>/dev/null      # comm 정확매칭 — 래퍼/스크립트 오살상 방지
sleep 3
kill "$VPID" "$PPID" "$SPID" 2>/dev/null
log "DONE"
