#!/bin/bash
# PTT 전용 오버나잇 안정성 시험 — g001 40명, floor 10s 무한순환. (VoLTE 제외)
#   deadline = 2026-06-08 07:00:00. crash 시 자동 재시작. 180s 샘플러 → report.jsonl.
#   중단: touch /home/cims/overnight_ptt/overnight.stop
#   ⚠️로그는 디스크(/home/cims, xfs) 기록 + cspsim 'RTP STATS' 고빈도 노이즈 grep 필터
#     (구버전이 /tmp tmpfs+usrquota 에 RTP STATS 폭증 → RAM잠식·EDQUOT 셸마비 사고 재발방지).
set -u
CIMS=/home/cims/work/cims
SIM="$CIMS/build/bin/cspsim"
DB=/opt/cims-agent/csp/config/csp.json
SRV=121.161.164.47
DIR=/home/cims/overnight_ptt
STOP=$DIR/overnight.stop
mkdir -p "$DIR"
rm -f "$STOP"
DEADLINE=$(date -d '2026-06-08 07:00:00' +%s)
REPORT="$DIR/report.jsonl"
PLOG="$DIR/ptt.log"
# 노이즈 필터: RTP per-packet + cspsim busy-spin 노이즈('not in call'/'rotation pass')도 제거
#   → cspsim 이 floor 순환 종료 후 잔여 spin 하더라도 로그 폭증 차단.
NOISE='RTP STATS|\[RTP|RTP recv|RTP send|not in call|rotation pass'
# floor 순환 라운드 수: cspsim 이 N라운드 후 정상 종료 → orchestrator 가 신선한 40명 통화 재수립
#   (구 -floor_loop 은 멤버 BYE 후 무한 busy-spin 결함 → -floor_rounds 로 깔끔한 주기적 재수립).
ROUNDS=3
echo "$$" > "$DIR/orchestrator.pid"

log(){ echo "[$(date '+%F %H:%M:%S')] $*" >> "$DIR/orchestrator.log"; }
log "START deadline=$(date -d @$DEADLINE '+%F %T') (PTT-only g001 40명/floor10s) DIR=$DIR"

ptt_loop(){
  local c=0
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    c=$((c+1)); log "PTT launch #$c"
    "$SIM" -server_ip "$SRV" -server_port 5060 -mode ptt -group g001 -scenario group-call \
      -domain ptt.mnc033.mcc450.3gppnetwork.org -count 40 -floor_hold 10 -floor_rounds "$ROUNDS" \
      -call_duration 1800 \
      -db "$DB" -no_video -media_dir "$CIMS/tests/media" 2>&1 \
      | grep --line-buffered -avE "$NOISE" >> "$PLOG"
    log "PTT exited #$c — restart in 5s"
    [ -f "$STOP" ] && break; sleep 5
  done
}

# backup watchdog: cspsim 이 (예외적으로) CPU 폭주 busy-spin 시 kill → ptt_loop 가 재시작.
#   정상 floor 순환 cspsim 은 저CPU(~25%); 스핀 시 단일코어 포화(>70%). 2회 연속 시 종료.
watchdog(){
  local hot=0 pid pc
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    sleep 45
    pid=$(pgrep -x cspsim | head -1)
    [ -z "$pid" ] && { hot=0; continue; }
    pc=$(ps -o %cpu= -p "$pid" 2>/dev/null | tr -d ' ' | cut -d. -f1)
    pc=${pc:-0}
    if [ "$pc" -ge 70 ]; then
      hot=$((hot+1))
      if [ "$hot" -ge 2 ]; then
        log "WATCHDOG: cspsim pid=$pid CPU=${pc}% busy-spin 의심 — kill (ptt_loop 재시작)"
        kill -9 "$pid" 2>/dev/null; hot=0
      fi
    else
      hot=0
    fi
  done
}

sampler(){
  local TOK; TOK=$(cat /tmp/oam_tok.txt 2>/dev/null)
  while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
    find "$DIR" -maxdepth 1 -name '*.log' -size +200M -exec truncate -s 0 {} \; 2>/dev/null
    local now p_alive p_join p_floor p_gnf p_410 leak deploys disk
    now=$(date '+%F %T')
    pgrep -x cspsim >/dev/null && p_alive=1 || p_alive=0
    p_join=$(grep -ac 'JOIN\|AFFILIATE' "$PLOG" 2>/dev/null); p_join=${p_join:-0}
    p_floor=$(grep -aic 'PTT Request\|floor hold\|grant' "$PLOG" 2>/dev/null); p_floor=${p_floor:-0}
    p_gnf=$(grep -aic 'group not found\|Group Not Found' "$PLOG" 2>/dev/null); p_gnf=${p_gnf:-0}
    p_410=$(grep -ac ' 410 \|status=410' "$PLOG" 2>/dev/null); p_410=${p_410:-0}
    disk=$(df -P "$DIR" | awk 'NR==2{print $5}')
    leak=$(curl -sk -m 5 -H "Authorization: Bearer $TOK" "https://127.0.0.1:4419/api/v1/stats/leak-reclaims" 2>/dev/null | head -c 300)
    deploys=$(curl -sk -m 5 -H "Authorization: Bearer $TOK" "https://127.0.0.1:4419/api/v1/deployments" 2>/dev/null \
      | python3 -c "import sys,json;d=json.load(sys.stdin);d=d if isinstance(d,list) else d.get('items',[]);print(','.join(f\"{x['process_name']}:{x.get('status')}\" for x in d if x['process_name'] in ('csp','cmp','csc')))" 2>/dev/null)
    printf '{"ts":"%s","ptt_alive":%s,"p_join":%s,"p_floor":%s,"p_410":%s,"p_gnf":%s,"disk":"%s","deploys":"%s","leak":%s}\n' \
      "$now" "$p_alive" "$p_join" "$p_floor" "$p_410" "$p_gnf" "$disk" "$deploys" "${leak:-null}" >> "$REPORT"
    sleep 180
  done
}

ptt_loop & PPID=$!
sampler  & SPID=$!
watchdog & WPID=$!
log "spawned ptt=$PPID sampler=$SPID watchdog=$WPID"

while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do sleep 30; done
log "deadline/stop reached — terminating load"
touch "$STOP"
pkill -x cspsim 2>/dev/null
sleep 3
kill "$PPID" "$SPID" "$WPID" 2>/dev/null
log "DONE"
