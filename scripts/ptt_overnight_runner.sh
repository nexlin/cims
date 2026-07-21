#!/bin/bash
# PTT 음성 그룹콜(g001) 무한 화자 로테이션 — 밤샘 안정성 시험 러너.
# cspsim -floor_loop 로 멤버 상주 + floor 무한 순환. 종료/크래시 시 자동 재시작.
# 중단: touch /tmp/ptt_overnight.stop  (또는 deadline 도달)
set -u
CIMS=/home/cims/work/cims
LOG=/tmp/ptt_overnight.log
STOP=/tmp/ptt_overnight.stop
DEADLINE=$(date -d '2026-06-02 09:30' +%s)
GROUP=g001
COUNT=5
FLOOR=5

cycle=0
echo "===== PTT overnight runner START $(date) deadline=$(date -d @$DEADLINE) group=$GROUP count=$COUNT floor=${FLOOR}s =====" >> "$LOG"
while [ "$(date +%s)" -lt "$DEADLINE" ] && [ ! -f "$STOP" ]; do
  cycle=$((cycle+1))
  echo "----- [cycle $cycle] $(date) launch cspsim -floor_loop -----" >> "$LOG"
  "$CIMS/build/bin/cspsim" -server_ip 121.161.164.47 \
    -db /opt/cims-agent/csp/config/csp.json -mode ptt -group "$GROUP" \
    -scenario group-call -count "$COUNT" -floor_hold "$FLOOR" -floor_loop \
    -domain ptt.mnc033.mcc450.3gppnetwork.org -media_dir "$CIMS/tests/media" -no_video \
    >> "$LOG" 2>&1
  rc=$?
  echo "----- [cycle $cycle] $(date) cspsim EXITED rc=$rc — restart in 5s -----" >> "$LOG"
  [ -f "$STOP" ] && break
  sleep 5
done
echo "===== PTT overnight runner DONE $(date) cycles=$cycle =====" >> "$LOG"
