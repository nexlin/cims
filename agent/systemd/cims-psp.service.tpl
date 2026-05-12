[Unit]
Description=CIMS PTT SIP Server (PSP)
After=network-online.target mariadb.service
Wants=network-online.target
# After=cims-redis.service              # 1.D-1 Redis register replication 도입 시 주석 해제

[Service]
Type=forking
User=${CIMS_USER}
Group=${CIMS_USER}
WorkingDirectory=${CIMS_HOME}
ExecStart=${CIMS_HOME}/cims.sh start psp
ExecStop=${CIMS_HOME}/cims.sh stop psp
PIDFile=${CIMS_HOME}/run/psp.pid
Restart=on-failure
RestartSec=5
TimeoutStartSec=30
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
