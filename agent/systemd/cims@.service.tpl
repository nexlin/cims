[Unit]
Description=CIMS Service (%i)
After=network-online.target mariadb.service
Wants=network-online.target
# After=cims-redis.service              # 1.D-1 Redis register replication 도입 시 주석 해제

[Service]
Type=forking
User=${CIMS_USER}
Group=${CIMS_USER}
WorkingDirectory=${CIMS_HOME}
ExecStart=${CIMS_HOME}/agent/bin/cims-svc start %i
ExecStop=${CIMS_HOME}/agent/bin/cims-svc stop %i
PIDFile=${CIMS_HOME}/run/%i.pid
Restart=on-failure
RestartSec=5
TimeoutStartSec=30
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
