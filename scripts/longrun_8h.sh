#!/bin/bash
# 8시간 장시간 안정성 시험 오케스트레이터 — VoLTE(1cps/ht20) + PTT(g001 40명 20s floor 무한순환).
#   각 부하는 crash/exit 시 자동 재시작. 180초 주기 health 샘플러가 report.jsonl 에 기록.
#   중단: touch /tmp/longrun_8h.stop  (또는 deadline 도달)
set -u
CIMS=/home/cims/work/cims
SIM="$CIMS/build/bin/cspsim"
DB=/opt/cims-agent/csp/config/csp.json
SRV=121.161.164.47
DIR=/tmp/longrun_8h
STOP=/tmp/longrun_8h.stop
DUR_SEC=${1:-28800}        # 기본 8h
mkdir -p "$DIR"
rm -f "$STOP"
START=$(date +%s)
DEADLINE=$((START + DUR_SEC))
REPORT="$DIR/report.jsonl"
VLOG="$DIR/volte.log"
PLOG="$DIR/ptt.log"
echo "$$" > "$DIR/orchestrator.pid"

log(){ echo "[$(date '+%H:%M:%S')] $*" >> "$DIR/orchestrator.log"; }
log "START dur=${DUR_SEC}s deadline=$(date -d @$DEADLINE '+%F %T')"

# ── VoLTE 부하: 1cps, ht20, 음성. crash 시 재시작 ──
volte_loop(){
  local c=0
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    c=$((c+1)); log "VoLTE launch #$c"
    "$SIM" -server_ip "$SRV" -server_port 5060 -mode volte -scenario call \
      -domain ims.mnc033.mcc450.3gppnetwork.org -cps 1 -ht 20 -calls 100000 \
      -db "$DB" -count 60 -db_offset 0 -no_video -media_dir "$CIMS/tests/media" \
      >> "$VLOG" 2>&1
    log "VoLTE exited #$c rc=$? — restart in 5s"
    [ -f "$STOP" ] && break; sleep 5
  done
}

# ── PTT 부하: g001 40명, floor 20s 무한순환. crash 시 재시작 ──
ptt_loop(){
  local c=0
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    c=$((c+1)); log "PTT launch #$c"
    "$SIM" -server_ip "$SRV" -server_port 5060 -mode ptt -group g001 -scenario group-call \
      -domain ptt.mnc033.mcc450.3gppnetwork.org -count 40 -floor_hold 20 -floor_loop \
      -db "$DB" -no_video -media_dir "$CIMS/tests/media" \
      >> "$PLOG" 2>&1
    log "PTT exited #$c rc=$? — restart in 5s"
    [ -f "$STOP" ] && break; sleep 5
  done
}

# ── health 샘플러: 180초 주기 report.jsonl ──
sampler(){
  local TOK; TOK=$(cat /tmp/oam_tok.txt 2>/dev/null)
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    local now v_alive p_alive v_end v_200 v_fail v_reg p_join p_grant p_410 p_gnf leak deploys
    now=$(date '+%F %T')
    pgrep -f 'cspsim.*mode volte' >/dev/null && v_alive=1 || v_alive=0
    pgrep -f 'cspsim.*mode ptt'   >/dev/null && p_alive=1 || p_alive=0
    v_end=$(grep -ac 'CALL ENDED' "$VLOG" 2>/dev/null || echo 0)
    v_200=$(grep -a 'CALL ENDED' "$VLOG" 2>/dev/null | grep -ac 'status=200' || echo 0)
    v_fail=$(grep -a 'CALL ENDED' "$VLOG" 2>/dev/null | grep -avc 'status=200' || echo 0)
    v_reg=$(grep -ac 'REGISTER FAILED' "$VLOG" 2>/dev/null || echo 0)
    p_join=$(grep -ac 'JOIN' "$PLOG" 2>/dev/null || echo 0)
    p_grant=$(grep -aic 'grant\|발언권 획득\|floor.*grant' "$PLOG" 2>/dev/null || echo 0)
    p_410=$(grep -ac '410' "$PLOG" 2>/dev/null || echo 0)
    p_gnf=$(grep -aic 'group not found\|GNF' "$PLOG" 2>/dev/null || echo 0)
    # CMP 누수 카운터 (정상=0). OAM API.
    leak=$(curl -sk -m 5 -H "Authorization: Bearer $TOK" "https://127.0.0.1:4419/api/v1/stats/leak-reclaims" 2>/dev/null | head -c 400)
    deploys=$(curl -sk -m 5 -H "Authorization: Bearer $TOK" "https://127.0.0.1:4419/api/v1/deployments" 2>/dev/null \
      | python3 -c "import sys,json;d=json.load(sys.stdin);d=d if isinstance(d,list) else d.get('items',[]);print(','.join(f\"{x['process_name']}:{x.get('status')}\" for x in d if x['process_name'] in ('csp','cmp','csc')))" 2>/dev/null)
    printf '{"ts":"%s","volte_alive":%s,"ptt_alive":%s,"v_end":%s,"v_200":%s,"v_fail":%s,"v_reg_fail":%s,"p_join":%s,"p_grant":%s,"p_410":%s,"p_gnf":%s,"deploys":"%s","leak":%s}\n' \
      "$now" "$v_alive" "$p_alive" "$v_end" "$v_200" "$v_fail" "$v_reg" "$p_join" "$p_grant" "$p_410" "$p_gnf" "$deploys" "${leak:-null}" >> "$REPORT"
    sleep 180
  done
}

volte_loop & VPID=$!
ptt_loop   & PPID=$!
sampler    & SPID=$!
log "spawned volte=$VPID ptt=$PPID sampler=$SPID"

# deadline 또는 stop 대기
while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do sleep 30; done
log "deadline/stop reached — terminating load"
touch "$STOP"
pkill -f 'cspsim.*mode volte' 2>/dev/null
pkill -f 'cspsim.*mode ptt'   2>/dev/null
sleep 3
kill "$VPID" "$PPID" "$SPID" 2>/dev/null
log "DONE"
