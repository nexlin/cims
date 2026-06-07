#!/bin/bash
# csp SIP 데드락 감시기 — SIP 5060 수신소켓이 안 비워지는 wedge 를 감지해 증거 캡처 + csp 재기동 복구.
#   배경: PTT REGISTER 버스트 하 csp DbManager 단일연결+락이 mysql_query 무한블록 시 전 SIP스레드 wedge.
#   csp 0.0.30 에 DB read/write/connect 타임아웃(5s) 추가로 무한블록→유한실패 전환했으나, 재발 검증/근본확정용.
set -u
DIR=/home/cims/overnight_ptt
EV=$DIR/deadlock_events.log
STOP=$DIR/overnight.stop
DEADLINE=$(date -d '2026-06-08 07:00:00' +%s)
TOK=$(cat /tmp/oam_tok.txt 2>/dev/null)
B=https://10.0.2.45:4419/api/v1
log(){ echo "[$(date '+%F %H:%M:%S')] $*" >> "$EV"; }
log "deadlock-watch START (SIP 5060 wedge 감시)"
hot=0
while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
  sleep 30
  # SIP 5060 수신큐 바이트(r). csp 가 드레인하면 작게 유지; wedge 면 buffer(4MB)까지 차고 안 빠짐.
  r=$(ss -uanm 2>/dev/null | grep -A1 ':5060' | grep -o 'r[0-9]*' | head -1 | tr -d 'r')
  r=${r:-0}
  if [ "$r" -gt 2000000 ]; then
    hot=$((hot+1))
  else
    hot=0
    continue
  fi
  [ "$hot" -lt 2 ] && continue   # 2회(60s) 연속 high 여야 wedge 확정(일시 버스트 제외)
  # ── WEDGE 확정 → 증거 캡처 ──
  CSPPID=$(pgrep -x csp | head -1)
  TS=$(date '+%Y%m%d_%H%M%S')
  CAP=$DIR/deadlock_${TS}
  log "WEDGE 감지: SIP r=$r pid=$CSPPID → 증거 캡처 $CAP.*"
  ss -uanm 2>/dev/null | grep -A1 ':5060' > "${CAP}.sock" 2>&1
  ps -L -o tid,pcpu,stat,wchan:40,comm -p "$CSPPID" > "${CAP}.threads" 2>&1   # ptrace 불필요
  # gdb 백트레이스 (ptrace_scope=0 또는 sudo 필요; 안되면 스킵)
  timeout 40 gdb -p "$CSPPID" -batch -ex "set pagination off" -ex "thread apply all bt" \
    > "${CAP}.gdb" 2>&1
  if grep -q "ptrace:" "${CAP}.gdb" 2>/dev/null; then
    log "  gdb attach 실패(ptrace_scope=1; '! sudo sysctl -w kernel.yama.ptrace_scope=0' 필요). wchan 캡처는 완료."
  else
    log "  gdb 백트레이스 캡처 완료 → ${CAP}.gdb"
  fi
  # ── 복구: csp(dep1 ctrl01) 재기동 ──
  log "  csp 재기동(OAM dep1 restart)으로 복구"
  curl -sk -m 10 -X POST -H "Authorization: Bearer $TOK" -H "Content-Type: application/json" \
    -d '{"job_type":"restart"}' "$B/deployments/1/job" >> "$EV" 2>&1
  echo "" >> "$EV"
  hot=0
  sleep 60   # 재기동 안정화 대기
done
log "deadlock-watch DONE"
