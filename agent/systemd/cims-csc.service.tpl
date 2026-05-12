[Unit]
Description=CIMS Mgmt API Server (CSC)
After=network-online.target mariadb.service
Wants=network-online.target

[Service]
Type=forking
User=${CIMS_USER}
Group=${CIMS_USER}
WorkingDirectory=${CIMS_HOME}
ExecStart=${CIMS_HOME}/cims.sh start csc
ExecStop=${CIMS_HOME}/cims.sh stop csc
PIDFile=${CIMS_HOME}/run/csc.pid
Restart=on-failure
RestartSec=5
TimeoutStartSec=30
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
