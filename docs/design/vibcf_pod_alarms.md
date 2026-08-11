# vIBCF/TrGW POD — 알람/Fault 메시지 카탈로그 (참고자료)

> 사내 타 시스템 **vIBCF/TrGW** 의 POD(알람/장애 메시지 정의서) v1.1.2 를 md 로 변환한
> **참고자료**다. 원본: `vIBCF_POD_1.1.2.docx` (원문 보존 변환 — CIMS 알람 체계의 정본이
> 아니며, CIMS 정본은 [alarm_standardization.md](alarm_standardization.md) ·
> [alarm_module_catalog.md](alarm_module_catalog.md) 다. 대조 관점은 표준화 §7.2 참조).


## 제 1장  ALARM / FAULT 메시지 설명


### ALARM 필드 설명

| 구분 | 의미 |
|---|---|
| 발생시간 | 장애가 발생한 시간 |
| 장애코드 | 시스템/프로세스 별로 부여된 장애코드 값(유일한 값) |
| 등급 | 장애 등급 (CRITICAL/MAJOR/MINOR/CLEARED) |
| 시스템 | 장애가 발생한 시스템(시스템명) |
| 장애장비 | 장애가 발생한 위치(서버명/프로세스명) |
| 장애타입 | 장애의 타입 |
| 장애설명 | 장애에 대한 설명 |
| 장애원인 | 장애 원인에 대한 짧은 설명 |
| 인지시간 | 장애를 인지한 시간 |
| 인지운영자 | 장애를 인지한 운영자 |


### ALARM 등급

| 구분 | 아이콘 |
|---|---|
| NORMAL |   |
| MINOR |   |
| MAJOR |   |
| CRITICAL |   |


### ALARM 타입

| 인덱스 | 타입 |
|---|---|
| 0 | CommunicationFail |
| 3 | DatabaseError |
| 7 | TcpCLosedError |
| 11 | ProcessKilled |
| 12 | CPUOverflow |
| 13 | DiskOverload |
| 14 | MemoryOverflow |
| 23 | StatusChange |
| 30 | QueueFull |
| 34 | NetworkFail |
| 39 | SuccessFailError |
| 40 | ChannelOverload |
| 41 | NTPDelayError |
| 42 | NTPOffsetError |
| 43 | NTPStatusError |
| 44 | NAScommunicationFail |
| 57 | OverloadDrop |
| 58 | CPSOverflow |
| 59 | SessionOverflow |
| 61 | ConnectionDeActive |
| 63 | HaStatusChange |
| 75 | SuccRateError |
| 76 | CommRateError |
| 77 | CompRateError |
| 78 | MediaTimeTooShort |
| 79 | MediaKbpsTooLow |
| 80 | SipOptionDeActive |
| 81 | SyntaxError |
| 83 | RTTError |
| 84 | SIPReasonOverflow |
| 85 | HaConfigChange |
| 86 | ProcessHangUp |
| 87 | GMResponseError |
| 88 | VMHostError |
| 89 | QueueOverflow |
| 90 | CDRDataEmpty |


### ALARM 종류

| NO | CODE | 알람 등급 | 발생 위치 | Alarm 타입 | 원인 |
|---|---|---|---|---|---|
| 1 | A0000 | 가변 | 각서버명/network | CommunicationFail | Physical network error |
| 2 | A0003 | 가변 | 프로세스명/DB/MySQL_xx | DatabaseError | MySQL_Error_num |
| 3 | A0007 | 가변 | 각서버명/프로세스명 | TcpCLosedError | TCP Connection Error |
| 4 | A0011 | 가변 | eMPXX_X/SARP | ProcessKilled | Process Killed |
| 5 | A0012 | Minor:70% Major:80% Critical:90% | 각서버명/CPU | CPUOverflow | CPU load is too high |
| 6 | A0013 | Minor:70% Major:80% Critical:90% | 각서버명/DISK// | DiskOverload | Used rate of DISK is too high |
| 7 | A0014 | Minor:70% Major:80% Critical:90% | 각서버명/MEMORY | MemoryOverflow | MEMORY load is too high |
| 8 | A0023 | 가변 | 각 서버명/프로세스명 | StatusChange | Block On |
| 9 | A0030 | 가변 | 각 서버명/프로세스명 | QueueFull | Queue Full Error |
| 10 | A0034 | 가변 | 각서버명/네트웍명 | NetworkFail | Ethernet is down |
| 11 | A0041 | 가변 | 각서버명/NTP/Delay | NTPDelayError | NTP Delay is too high |
| 12 | A0042 | 가변 | 각서버명/NTP/Offset | NTPOffsetError | NTP Offset is too high |
| 13 | A0043 | 가변 | 각서버명/NTP/ntpd | NTPStatusError | ntpd status error |
| 14 | A0057 | 가변 | 장애발생서버/장애원인 | OverloadDrop | Overload Error |
| 15 | A0058 | 가변 | CM/CPS | CPSOverflow | CPS is too high |
| 16 | A0059 | 가변 | 각서버명/노드명(노드ID) | SessionOverflow | Node Session usage is too high |
| 17 | A0061 | 가변 | 각 서버명/각 프로세스명 | ConnectionDeActive | ConnectionDeActive |
| 18 | A0063 | 가변 | 각 서버명/HA | HaStatusChange | Monitor PROC Current Stopped |
| 19 | A0075 | 가변 | 시스템명/NODE(XXXX) | SuccRateError | Success rate is too low |
| 20 | A0076 | 가변 | 시스템명/NODE(XXXX) | CommRateError | Communication rate is too low |
| 21 | A0077 | 가변 | 시스템명/NODE(XXXX) | CompRateError | Completion rate is too low |
| 22 | A0078 | 가변 | 시스템명/NODE(XXXX) | MediaTimeTooShort | Media Time is too short |
| 23 | A0079 | 가변 | 시스템명/NODE(XXXXX)/AUDIO_TX | MediaKbpsTooLow | Kbps is too low |
| 24 | A0081 | 가변 | CM/CCM/SIP_SYNTAX | SyntaxError | SIP Message Syntax Error |
| 25 | A0083 | 가변 | 시스템명/NODE(XXXX)/RTT_종류 | RTTError | RTT(종류) is too high |
| 26 | A0084 | 가변 | 시스템명/NODE(노드ID)/XXXX | SIPReasonOverflow | XXXX Count is too high |
| 27 | A0085 | 가변 | 시스템명/HA | HaConfigChange | mandotory ON |
| 28 | A0086 | 가변 | 시스템명/프로세스명 | ProcessHangUp | Hang Up Detected |
| 29 | A0087 | 가변 | 시스템명/VIMS | GMResponseError | GM Response Fail(EMS <-> GM) |
| 30 | A0088 | 가변 | 시스템명/VIMS | VMHostError | VM HOST Response Fail(EMS <-> GM) |
| 31 | A0089 | Minor:70% Major:80% Critical:90% | 시스템명/프로세스명/스레드명 | QueueOverflow | Queue load is too high |
| 32 | A0090 | 가변 | 시스템명/LB/CHARGE | CDRDataEmpty | CDR DATA Not Exist |


### FAULT 필드 설명

| 구분 | 의미 |
|---|---|
| 발생시간 | 장애가 발생한 시간 |
| 코드 | 시스템/프로세스 별로 부여된 장애코드 값(유일한 값) |
| 등급 | 등급 (FAULT/WARNING/INFO) |
| 장비위치 | 발생한 위치(서버명/프로세스명) |
| 로그 | FAULT 메시지 내용 |


### FAULT 종류

| NO | CODE | 등급 | 발생 위치 | 로그 |
|---|---|---|---|---|
| 1 | F4001 | FAULT | CM/CCM | Session ID, SIP method와 response code, 수신 address 정보 |
| 2 | F4002 | FAULT | CM/CCM | SIP 전체 메시지 및 Syntax Error에 대한 상세 정보를 표시함 |
| 3 | F4006 | FAULT | CM/CCM | SIP method와 response code, 수신 address 정보 |
| 4 | F4009 | FAULT | CM/CCM | SIP method와 response code, 수신 address 정보 |
| 5 | F400A | FAULT | CM/CCM | 현재 Dialog 개수, 최대 Dialog 개수, SIP method와 response code, 수신 address 정보 |
| 6 | F400B | FAULT | CM/CCM | SIP method와 response code, 수신 address 정보 |
| 7 | F400C | FAULT | CM/CCM | Dialog정보, 현재 FSM state, SIP method와 response code, 수신 address 정보 |
| 8 | F400E | FAULT | CM/CCM | Route ID, SIP method와 response code, 수신 address 정보 |
| 9 | F4039 | FAULT | CM/CCM | Dialog 정보, Expire 시간 |
| 10 | F403C | FAULT | CM/CCM | Dialog 정보, Expire 시간 |
| 11 | F403D | FAULT | CM/CCM | Dialog 정보, Expire 시간 |
| 12 | F403E | FAULT | CM/CCM | Dialog 정보, Expire 시간, SIP method와 response code |
| 13 | F4100 | FAULT | CM/CCM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 14 | F4101 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 15 | F4102 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 16 | F4200 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 17 | F4300 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 18 | F4301 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 19 | F4302 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 20 | F4303 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 21 | F4304 | FAULT | CCM_0X/CM | 인입된 세션의 세션정보 및 내용을 표시함. |
| 22 | F5001 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 23 | F5002 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 24 | F5003 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 25 | F5004 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 26 | F5005 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 27 | F5006 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 28 | F5007 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 29 | F5008 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 30 | F5009 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 31 | F500A | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 32 | F500B | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 33 | F500C | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 34 | F500D | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 35 | F500E | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 36 | F500F | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 37 | F5011 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 38 | F5012 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 39 | F5013 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 40 | F5014 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 41 | F5015 | FAULT | eMP0X_X/TGAS | 인입된 세션의 세션정보 및 내용을 표시함. |
| 42 | S1003 | INFO | OMP | COMPLETED WRITING 1MIN STATISTICS (루트<CDR>) - CM01 |
| 43 | S1007 | INFO | OMP | COMPLETED WRITING 1MIN STATISTICS (RTT 과금) - CM01 |
| 44 | S1008 | INFO | OMP | COMPLETED WRITING 1MIN STATISTICS (RTT SIP) - CM01 |
| 45 | S1101 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (시스템) |
| 46 | S1102 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (장애) |
| 47 | S1103 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (루트<CDR>) - CM01 |
| 48 | S1104 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (호<SIP>) - CM01 |
| 49 | S1105 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (과금) - CM01 |
| 50 | S1106 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (MEDIA) - CM01 |
| 51 | S1107 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (RTT 과금) - CM01 |
| 52 | S1108 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (RTT SIP) - CM01 |
| 53 | S1109 | INFO | OMP | COMPLETED WRITING 5MIN STATISTICS (SIP REASON) - CM01 |
| 54 | S1201 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (시스템) |
| 55 | S1202 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (장애) |
| 55 | S1203 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (루트<CDR>) |
| 56 | S1204 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (호<SIP>) |
| 57 | S1205 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (과금) - CM01 |
| 58 | S1206 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (MEDIA) - CM01 |
| 59 | S1207 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (RTT 과금) - CM01 |
| 60 | S1208 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (RTT SIP) - CM01 |
| 61 | S1209 | INFO | OMP | COMPLETED WRITING HOUR STATISTICS (SIP REASON) - CM01 |
| 62 | S1301 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (시스템) |
| 63 | S1302 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (장애) |
| 64 | S1303 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (루트<CDR>) |
| 65 | S1304 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (호<SIP>) |
| 66 | S1305 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (과금) - CM01 |
| 67 | S1306 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (MEDIA) - CM01 |
| 68 | S1307 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (RTT 과금) - CM01 |
| 69 | S1308 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (RTT SIP) - CM01 |
| 70 | S1309 | INFO | OMP | COMPLETED WRITING DAY STATISTICS (SIP REASON) - CM01 |
| 71 | S1401 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (시스템) |
| 72 | S1402 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (장애) |
| 73 | S1403 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (루트<CDR>) |
| 74 | S1404 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (호<SIP>) |
| 75 | S1405 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (과금) - CM01 |
| 76 | S1406 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (MEDIA) - CM01 |
| 77 | S1407 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (RTT 과금) - CM01 |
| 78 | S1408 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (RTT SIP) - CM01 |
| 79 | S1409 | INFO | OMP | COMPLETED WRITING WEEK STATISTICS (SIP REASON) - CM01 |
| 80 | S1501 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (시스템) |
| 81 | S1502 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (장애) |
| 82 | S1503 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (루트<CDR>) |
| 83 | S1504 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (호<SIP>) |
| 84 | S1505 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (과금) - CM01 |
| 85 | S1506 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (MEDIA) - CM01 |
| 86 | S1507 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (RTT 과금) - CM01 |
| 87 | S1508 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (RTT SIP) - CM01 |
| 88 | S1509 | INFO | OMP | COMPLETED WRITING MONTH STATISTICS (SIP REASON) - CM01 |
| 89 | S1601 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (시스템) |
| 90 | S1602 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (장애) |
| 91 | S1603 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (루트<CDR>) |
| 92 | S1604 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (호<SIP>) |
| 93 | S1605 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (과금) - CM01 |
| 94 | S1606 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (MEDIA) - CM01 |
| 95 | S1607 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (RTT 과금) - CM01 |
| 96 | S1608 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (RTT SIP) - CM01 |
| 97 | S1609 | INFO | OMP | COMPLETED WRITING YEAR STATISTICS (SIP REASON) - CM01 |
| 98 | S1901 | INFO | OMP | OMP01 : MAX_CPU = 7, CPU = 6, MAX_Memory = 21, Memory = 21 |
| 99 | S1902 | INFO | OMP | REQUEST = 0, SUCCESS = 0, FAIL = 0, USAGE = 0 (루트<CDR>) |
| 100 | S1903 | INFO | OMP | 성공률 = 0, 소통률 = 0, 완료율 = 0 (호<SIP>) |
| 101 | S1904 | INFO | OMP | NODE ID = 1111, OPTIONS RX = 0, TX = 0 (OPTIONS 장애 체크<SIP>) |
| 102 | S1905 | INFO | OMP | S1005, IBC52, 성공률 = 70% |
| 103 | S1906 | INFO | OMP | S1006, IBC52, IN RTT(SIP) = 200 |
| 104 | S1907 | INFO | OMP | S1007, IBC52, OUT RTT(SIP) = 200 |
| 105 | S1908 | INFO | OMP | S1008, IBC52, CGName = imscg1, RTT = 200 |
| 106 | S2101 | INFO | OMP | FINISHED PKG BACKUP (CM01) RESULT = SUCCESS |
| 107 | S2102 | INFO | OMP | FINISHED PKG BACKUP (CM01) RESULT = FAIL |
| 108 | S2103 | INFO | OMP | SENDED TO OMP PKG BACKUP (CM01) RESULT = SUCCESS |
| 109 | S2104 | INFO | OMP | SEND TO OMP PKG BACKUP (CM01) RESULT = FAIL, rc = %d |
| 110 | S2105 | INFO | OMP | START PKG AUTO BACKUP (CM01) |
| 111 | S2106 | INFO | OMP | FINISHED PKG AUTO BACKUP (CM01) RESULT = SUCCESS |
| 112 | S2107 | INFO | OMP | FINISHED PKG AUTO BACKUP (CM01) RESULT = FAIL |
| 113 | S2108 | INFO | OMP | SEND TO OMP PKG AUTO BACKUP (CM01) RESULT = SUCCESS |
| 114 | S2109 | INFO | OMP | SEND TO OMP PKG AUTO BACKUP (CM01) RESULT = FAIL |
| 115 | S2201 | INFO | OMP | FINISHED DB BACKUP RESULT = SUCCESS |
| 116 | S2202 | FAULT | OMP | FINISHED DB BACKUP RESULT = FAIL |
| 117 | S2203 | INFO | OMP | FINISHED DB RESTORE RESULT = SUCCESS |
| 118 | S2204 | INFO | OMP | FINISHED DB RESTORE RESULT = FAIL |
| 119 | S2205 | INFO | OMP | START DB AUTO BACKUP |
| 120 | S2206 | INFO | OMP | FINISHED DB AUTO BACKUP RESULT = SUCCESS |
| 121 | S2207 | FAULT | OMP | FINISHED DB AUTO BACKUP RESULT = FAIL |
| 122 | S3001 | INFO | OMP | HA DACT SUCCESS |
| 123 | S3002 | INFO | OMP | HA DACT FAIL |
| 124 | S3003 | WARN | OMP | ACTIVE SYSTEM CHANGED [SERVER01(S), SERVER02(A)] |
| 125 | S3004 | WARN | OMP | SEND DACT MESSAGE CPU ( 80 > 70 ) Server = CM Server |
| 126 | S3005 | WARN | OMP | SEND DACT MESSAGE MEM ( 90 > 80 ) Server = CM Server |


## 제 2장  Alarm


### A0000


**메시지 설명**

설정된 임계치보다 CPU 사용률이 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0000 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/Network |
| 장애 타입 | CommunicationFail |
| 장애 원인 | Physical network error ip:xxx.xxx.xxx.xxx |
| 장애 설명 | Physical ethernet link to the server failed |


**조치사항**

2.1) 발생 위치를 확인하여 해당 Alarm 이 발생한 장비에서 네트웍인터페이스 상태를 확인한다.

2.2) 네트웍선이나 포트 상태를 점검한다.


### A0003


**메시지 설명**

EMS의 MySQL 사용 프로세스들에서 Query관련 Syntax Error가 나왔을 경우에 알람 발생

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0003 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 프로세스명/DB/MySQL_xx |
| 장애 타입 | DatabaseError |
| 장애 원인 | MySQL syntax 에러의 내용이 기입됨 |
| 장애 설명 | DB Error, detected by 프로세스명 |


**조치사항**

각 프로세스의 로그 (~/ibc/log/프로세스명)에서 해당 Query를 확인한다.


### A0007


**메시지 설명**

내부 IPC 네트워크에 문제가 생기거나 Process Kill 되어 내부 연동이 되지 않을 경우  발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0007 |
| 장애 등급 | CLEARED, CRITICAL (가변) |
| 발생 위치 | 각서버명/프로세스명 |
| 장애 타입 | TcpCLosedError |
| 장애 원인 | TCP Connection Error |
| 장애 설명 | TcpCLosedError |


**조치사항**

2.1). 각 서버에서 내부 IPC 망 간의 Ping 확인하여 이상있을 시 조치한다.

2.2). 프로세서가 정상적으로 구동되어 있는지 확인하여 이상있을 시 조치한다.


### A0011


**메시지 설명**

eMP 장비 SARP 프로세스가 Down 됐을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0011 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | eMP0X_X/SARP |
| 장애 타입 | ProcessKilled |
| 장애 원인 | Process Killed |
| 장애 설명 | Process Killed |


**조치사항**

2.1) 프로세스의 로그(~/log/SARP/SARP.mmdd)를 확인한다.


### A0012


**메시지 설명**

설정된 임계치보다 CPU 사용률이 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0012 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/CPU |
| 장애 타입 | CPUOverflow |
| 장애 원인 | CPU load is too high |
| 장애 설명 | CPU load is A% (CRI:B,MAJ:C,MIN:D) |

A : 현재 CPU 사용률

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) 발생 위치를 확인하여 해당 Alarm 이 발생한 장비에서 CPU 사용률을 확인한다.

2.2) CPU 부하가 많이 발생한 프로세스의 로그(~/ibc/log/프로세스명/프로세스명.mmdd)를 확인한다.


### A0013


**메시지 설명**

설정된 임계치보다 DISK 사용률이 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0013 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/DISK/파티션명 |
| 장애 타입 | DiskOverload |
| 장애 원인 | Used rate of DISK is too high |
| 장애 설명 | used amount of [A] is B% (CRI:C,MAJ:D,MIN:E) |

A : 파티션 명

B : 현재 DISK 사용률

C : CRITICAL 등급 임계치

D : MAJOR 등급 임계치

E : MINOR 등급 임계치


**조치사항**

2.1) 발생 위치를 확인하여 해당 Alarm 이 발생한 장비에서 다스크 사용률을 확인 한다.

2.2) 장애가 발생한 파티션에 로그를 확인하고 로그가 보관기간이 지난 로그가 있는지 확인한다.

2.3) 보관기간이 지난 로그가 있을 경우 DELLOG 프로세스의 로그를 확인한다.

2.4) 보관기간이 지난 로그가 없을 경우 DELLOG 설정파일 (~/ibc/config/DELLOG/DELLOG.cfg) 에서 보관주기를 조절한다.


### A0014


**메시지 설명**

설정된 임계치보다 MEMORY 사용률이 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0014 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/MEMORY |
| 장애 타입 | MemoryOverflow |
| 장애 원인 | MEMORY load is too high |
| 장애 설명 | MEMORY load is A% (CRI:B,MAJ:C,MIN:D) |

A : 현재 MEMORY 사용률

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) 발생 위치를 확인하여 해당 Alarm 이 발생한 장비에서 MEMORY 사용률을 확인한다.

2.2) MEMORY 부하가 많이 발생한 프로세스의 로그(~/ibc/log/프로세스명/프로세스명.mmdd)를 확인한다.

2.3) 로그에 이상이 없을 경우 해당 프로세스의 메모리 릭이 존재하는지 검사한다.


### A0023


**메시지 설명**

각 프로세서에서 관리하고 있는 노드가 Block될 경우 발생한다.

SLB : CM으로 호 유입되지 않도록 Manual Block 기능이 ON되었을 시에 발생한다.

CM : EMP로 호 유입되지 않도록 Manual Block 기능이 ON 되었을 시에 발생한다.

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0023 |
| 장애 등급 | CLEARED, CRITICAL (가변) |
| 발생 위치 | 각 서버명/프로세스명 |
| 장애 타입 | StatusChange |
| 장애 원인 | Block On |
| 장애 설명 | 해당서버-대상노드 MBlock On |


**조치사항**

2.1) SLB 프로세서에서 Manual Block 알람이 발생한 경우

- Manual Block 되어있는 CM 노드가 정상적으로 기동되어 호를 유입시켜도 처리할 수 있는 상태인지 점검한다.

- MMI Client에서 VNODE 관리 -> SLB 변경을 통해 Manual Block을 해제한다.

2.2) CM 프로세서에서 Manual Block 알람이 발생한 경우

- Manual Block 되어있는 EMP 노드가 정상적으로 기동되어 호를 유입시켜도 처리할 수 있는 상태인지 점검한다.

- MMI Client에서 VNODE 관리 -> CCM 변경을 통해 Manual Block을 해제한다.


### A0030


**메시지 설명**

프로세스내 Queue 가 Full이 났을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0030 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/프로세스명 |
| 장애 타입 | QueueFull |
| 장애 원인 | Queue Full Error Queue Full Cleared Heartbeat Timeout Fail Heartbeat Timeout Cleared |
| 장애 설명 | Queue[CCM_dump] Full. Current[2000], Max[2000]. Drop=1 Queue Full. Drop=710. Cleared eMP01_1:50.1.17.138 Heartbeat Timeout |


**조치사항**

2.1) 발생 위치를 확인하여 해당 Alarm 이 발생한 프로세스의 로그(~/ibc/log/프로세스명/프로세스명.mmdd)를 확인한다.


### A0034


**메시지 설명**

네트웍 인터페이스에 문제가 생겼을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0035 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/네트웍인터페이스명 |
| 장애 타입 | NetworkFail |
| 장애 원인 | Ethernet is down |
| 장애 설명 | A is down |

A : 네트웍인터페이스명


**조치사항**

2.1) 발생 위치를 확인하여 해당 Alarm 이 발생한 장비에서 네트웍인터페이스 상태를 확인한다.

2.2) 네트웍선이나 포트 상태를 점검한다.


### A0041


**메시지 설명**

설정된 임계치보다 NTP Delay가 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0041 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각서버명/NTP/Delay |
| 장애 타입 | NTPDelayError |
| 장애 원인 | NTP Delay is too high |
| 장애 설명 | NTP Delay is A (CRI:B) |

A : 현재 NTP Delay

B : 설정된 임계치


**조치사항**

2.1) GUI 메인메뉴 [기타]-[NTP 조회] 를 통해 NTP 연동 상태를 확인한다.

2.2) NTP 서버에 문제가 있는지 확인한다.

2.3) [장애관리]-[장애설정] 에서 NTP delay 임계치를 조절하여 장애를 해제시킨다


### A0042


**메시지 설명**

설정된 임계치보다 NTP Offset이 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0042 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각서버명/NTP/Offset |
| 장애 타입 | NTPOffsetError |
| 장애 원인 | NTP Offset is too high |
| 장애 설명 | NTP Offset is A (CRI:B) |

A : 현재 NTP Offset

B : 설정된 임계치


**조치사항**

2.1) GUI 메인메뉴 [기타]-[NTP 조회] 를 통해 NTP 연동 상태를 확인한다.

2.2) NTP 서버에 문제가 있는지 확인한다.

2.3) [장애관리]-[장애설정] 에서 NTP delay 임계치를 조절하여 장애를 해제시킨다


### A0043


**메시지 설명**

NTP 데몬에 문제가 생겼을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0043 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각서버명/NTP/ntpd |
| 장애 타입 | NTPStatusError |
| 장애 원인 | NTP Status Error |
| 장애 설명 | NTPD Connection Refused |


**조치사항**

2.1) GUI 메인메뉴 [기타]-[NTP 조회] 를 통해 NTP 연동 상태를 확인한다.

2.2) 이상이 있을 경우 해당 장비로 접속하여 ntpd 상태를 확인한다.


### A0057


**메시지 설명**

CPU, MEMORY 등 내부 자원 혹은, 인입되는 CPS가 Overload 되어 Drop 되는 호가 생길 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0057 |
| 장애 등급 | CLEARED, CRITICAL (가변) |
| 발생 위치 | 장애발생서버/장애원인 |
| 장애 타입 | OverloadDrop |
| 장애 원인 | 장애 원인 Overload Error |
| 장애 설명 | 장애 원인 Overload Error. 현재상태, 설정값, Drop호 표시 |


**조치사항**

2.1) CPU, MEMORY 에 대해 발생할 경우

- GUI 메인화면에서 CPU, MEMORY 상태를 확인한다.

2.2) CPS 에 대해 발생할 경우

- GUI 메인화면에서 CPS 상태를 확인한다.

- 인입되는 호가 성능 이상으로 유입되므로 조정한다.


### A0058


**메시지 설명**

설정된 임계치보다 CPS 가 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0058 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | CM/CPS |
| 장애 타입 | CPSOverflow |
| 장애 원인 | CPS is too high |
| 장애 설명 | Current CPS is A (CRI:B,MAJ:C,MIN:D) |

A : 현재 CPS

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) GUI 메인화면에서 CPS 상태를 확인한다.

2.2) 시스템이 수용할 수 있는 CPS 을 초과할 경우 과부하 제어 등을 통해 CPS 를 낮춘다.


### A0059


**메시지 설명**

설정된 임계치보다 VC 사용률이 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0059 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/노드명(노드 ID) |
| 장애 타입 | SessionOverflow |
| 장애 원인 | Node Session usage is too high |
| 장애 설명 | Current Node Session usage is A% (CRI:B,MAJ:C,MIN:D) |

A : 현재 세션 사용률

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) GUI 메인화면 세션상태 탭에서 세션 점유율을 확인한다.

2.2) [장애관리]-[장애설정] 에서 점유율 임계치를 조절하여 장애를 해제시킨다.

2.3) 시스템이 수용가능한 세션을 초과할 경우 과부하 제어 등을 통해 세션 점유율을 낮춘다.


### A0061


**메시지 설명**

각 프로세서에서 관리하고 있는 노드와의 연동이 끊어질 경우 발생함

SLB : 외부 노드(MSS 및 IBCF)와 연동이 끊어질 경우.

CCM : EMP의 TGAS 프로세서와 연동이 끊어질 경우.

CDP : 과금 서버와 연동이 끊어질 경우.

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0061 |
| 장애 등급 | CLEARED , CRITICAL (가변) |
| 발생 위치 | 각 서버명/각 프로세스명 |
| 장애 타입 | ConnectionDeActive |
| 장애 원인 | ConnectionDeActive |
| 장애 설명 | Communication Failed |


**조치사항**

2.1) SLB 프로세서에서 발생할 경우

- MMI Client에서 국데이터관리 -> SIP Server관리 -> SIP Server 조회 를 통해 SIP Local 상태를 확인한다.

- Active LB서버에서 해당 Local정보대로 Bind되어 있는지 확인한다.

(netstat -anlp | grep SLB)

- Active LB 서버에서 대상 노드(MSS 혹은 IBCF)와의 Ping을 통해 네트워크 상태 를 확인한다.

- GUI Client에서 통계관리 -> 통계조회 -> SIP 통계 에서 OPTIONS 메시지의 수신 상태를 조회한다.

- Tcpdump를 통해 대상 노드로 OPTIONS 메시지를 정상적으로 전송하는지, 수신되는 응답 메시지가 있는지 확인한다.

2.2) CCM 프로세서에서 발생할 경우

- GUI Client의 프로세스 상태 창을 통해 EMP 의 TGAS 프로세서가 정상구동되어 있는지 확인한다.

- IPC 내부 서버 간의 TCP 네트워크 상태를 확인한다.

2.3) CDP 프로세서에서 발생할 경우

- 과금 서버 와의 SCTP 네트워크 상태를 확인한다.

- TCP DUMP를 통해 전송되는 메시지와 과금 서버의 응답 메시지가 정상적인지 확인한다.


### A0063


**메시지 설명**

HA 절체시 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0063 |
| 장애 등급 | CLEARED , MAJOR (가변) |
| 발생 위치 | 각 서버명/HA |
| 장애 타입 | HaStatusChange |
| 장애 원인 | Monitor PROC Current Stopped Manual Switch Active Change Clear Memory load is too high CPU load is too high Initiate VIP Status:DOWN Bothside ACTIVE ==> O:STA P:ACT Mandatory Set:STANDY ==> O:STA PING failure |
| 장애 설명 | Changeover (ACTIVE->STANDBY) Changeover cleared. |


**조치사항**

2.1) 장애원인을 통해 절체 원인을 파악한다.


### A0075


**메시지 설명**

성공률이 설정된 임계치보다 낮을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0075 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 시스템명/NODE(XXXX) |
| 장애 타입 | SuccRateError |
| 장애 원인 | Success rate is too low |
| 장애 설명 | Success rate of [NODE(XXXX)] is A% (CRI:B,MAJ:C,MIN:D) |

A : 현재 성공률

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) CDR 통계에서 REQUEST,  FAIL 필드를 통해 성공률을 확인한다.

2.2) CDR 통계에서 실패원인 필드를 확인한다.

2.3) CDR 상세 조회 및 호추적을 통해 실패 원인을 확인한다.


### A0076


**메시지 설명**

소통률이 설정된 임계치보다 낮을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0076 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 시스템명/NODE(XXXX) |
| 장애 타입 | CommRateError |
| 장애 원인 | Communication rate is too low |
| 장애 설명 | Communication rate of [NODE(XXXX)] is A% (CRI:B,MAJ:C,MIN:D) |

A : 현재 소통률

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) CDR 통계에서 소통률을 확인한다.

2.2) CDR 통계에서 실패원인 필드를 확인한다.

2.3) CDR 상세 조회 및 호추적을 통해 실패 원인을 확인한다


### A0077


**메시지 설명**

완료률이 설정된 임계치보다 낮을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0077 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 시스템명/NODE(XXXX) |
| 장애 타입 | CompRateError |
| 장애 원인 | Completion rate is too low |
| 장애 설명 | Completion rate of [NODE(XXXX)] is A% (CRI:B,MAJ:C,MIN:D) |

A : 현재 완료률

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) CDR 통계에서 완료률을 확인한다.

2.2) CDR 통계에서 실패원인 필드를 확인한다.

2.3) CDR 상세 조회 및 호추적을 통해 실패 원인을 확인한다..


### A0078


**메시지 설명**

미디어 Time 이 설정된 임계치보다 낮을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0078 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 시스템명/NODE(XXXX) |
| 장애 타입 | MediaTimeTooShort |
| 장애 원인 | Media Time is too short |
| 장애 설명 | Media Time of [NODE(XXXX)] is A (CRI:B,MAJ:C,MIN:D) |

A : 현재 미디어 Time

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) 미디어 통계에서 미디어 연결시간(초)를 확인한다.

2.2) CDR 통계에서 실패원인 필드를 확인한다.

2.3) CDR 상세 조회 및 호추적을 통해 원인을 확인한다.


### A0079


**메시지 설명**

미디어 KBps 가 설정된 임계치보다 낮을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0079 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 시스템명/NODE(XXXX)/(AUDIO/VIDEO)_(RX/TX) |
| 장애 타입 | MediaKbpsTooLow |
| 장애 원인 | Kbps is too low |
| 장애 설명 | Kbps of [NODE(XXXX)] is A (CRI:B,MAJ:C,MIN:D) |

A : 현재 Kbps

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) CDR 통계에서 Kbps를 확인한다.

2.2) CDR 통계에서 실패원인 필드를 확인한다.

2.3) CDR 상세 조회 및 호추적을 통해 실패 원인을 확인한다.


### A0081


**메시지 설명**

SIP Syntax Error 가 생겼을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0081 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | CM/CCM/SIP_SYNTAX |
| 장애 타입 | SyntaxError |
| 장애 원인 | Kbps is too low |
| 장애 설명 | Warning: SYNTAX_FAIL Message was received |


**조치사항**

2.1) 장애가 발생한 CM장비에서 CCM 프로세스 로그(~/ibc/log/CCM/)를 통해 원인을 확인한다.

2.2) CDR 상세 조회 및 호추적을 통해 실패 원인을 확인한다.


### A0083


**메시지 설명**

SIP/과금 RTT 값이 설정된 임계치 보다 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0083 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 시스템명/NODE(노드ID)/A |
| 장애 타입 | SyntaxError |
| 장애 원인 | RTT(B) is too high |
| 장애 설명 | RTT(B) is C (CRI:D) TIME(E) |

A : RTT_SIP 또는 RTT_CHARGE

B : SIP 또는 CHARGE

C : RTT 값

D : RTT 임계치 값

E : 장애 체크 시간, 포멧 = hh:mm


**조치사항**

2.1) RTT 통계에서 RTT 값을 확인한다.

2.2) CDR 상세 조회 및 호추적을 통해 원인을 확인한다.


### A0084


**메시지 설명**

SIP 실패 이유가 설정된 임계치 값을 초과했을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0084 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 시스템명/NODE(노드ID)/A |
| 장애 타입 | SIPReasonOverflow |
| 장애 원인 | A Count is too high |
| 장애 설명 | A Count is 11, ( Check Count:B, CRI:C, MAJ:D, MIN:E) TIME(F) |

A : SIP Reason

B : 장애 체크 최소 개수

C~E : CRITICAL / MAJOR / MINOR 임계치 값

F : 장애 체크 시간


**조치사항**

2.1) 통계를 통해 해당 노드의 실패 이유를 확인한다.

2.2) 호이력 조회 및 호추적을 통해 원인을 파악한다.


### A0085


**메시지 설명**

HA 설정이 변경되었을 경우 발생

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0085 |
| 장애 등급 | CLEARED, MAJOR (가변) |
| 발생 위치 | 시스템명/HA |
| 장애 타입 | HaConfigChange |
| 장애 원인 | mandotory ON Changeover ON Set Changeover OFF Set |
| 장애 설명 | mandatory ON. Changeover ON Set Changeover OFF Set |


**조치사항**

2.1) 장애 원인을 통해 변경 내역을 확인한다.


### A0086


**메시지 설명**

Process가 Hang상태일 경우 발생

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0086 |
| 장애 등급 | CLEARED, CRITICAL (가변) |
| 발생 위치 | 시스템명/프로세스명 |
| 장애 타입 | ProcessHangUp |
| 장애 원인 | Hang Up Detected |
| 장애 설명 | WORKER Hang Up Detected. MONITOR_TIME(1000)ms, LIMIT(5), HA(1), RESTART(0) WORKER Hang Up Detected. MONITOR_TIME(1000)ms, LIMIT(5), HA (0), RESTART(1) |


**조치사항**

2.1) 장애 원인을 통해 변경 내역을 확인한다.


### A0087


**메시지 설명**

VIMS가 GM가 연동시도 중 응답이 없는 경우

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0087 |
| 장애 등급 | CLEARED, MAJOR (가변) |
| 발생 위치 | 시스템명/VIMS/연동명 |
| 장애 타입 | GMResponseError |
| 장애 원인 | GM Response Fail(EMS <-> GM) |
| 장애 설명 | Failed to Response Message (INDICATOR) Success Failed to Response Message (STATUS) Bad Request Failed to Response Message (INIT_VNF) Unauthorized Failed to Response Message (FAULT) Not Found Failed to Response Message (SUBSC_CRT) Internal Server Error Failed to Response Message (SUBSC_UDT) TimeOut |


**조치사항**

2.1) 장애가 발생한 장비에서 VIMS 로그(~/ibc/log/프로세스명/)를 통해 원인을 확인한다.


### A0088


**메시지 설명**

VIMS가 VM HOST에 연동시도 중 응답이 없는 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0088 |
| 장애 등급 | CLEARED, CRITICAL (가변) |
| 발생 위치 | 시스템명/VIMS/연동명 |
| 장애 타입 | VMHostError |
| 장애 원인 | VM Host Response Fail(EMS <-> GM) |
| 장애 설명 | Failed to Response Message (INDICATOR) Success Failed to Response Message (STATUS) Bad Request Failed to Response Message (INIT_VNF) Unauthorized Failed to Response Message (FAULT) Not Found Failed to Response Message (SUBSC_CRT) Internal Server Error Failed to Response Message (SUBSC_UDT) TimeOut |


**조치사항**

2.1) 장애가 발생한 장비에서 VIMS 로그(~/ibc/log/프로세스명/)를 통해 원인을 확인한다.


### A0089


**메시지 설명**

설정된 임계치보다 QUEUE 사용률이 높을 경우 발생함

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0089 |
| 장애 등급 | CLEARED, MINOR, MAJOR, CRITICAL (가변) |
| 발생 위치 | 각 서버명/프로세스명/스레드명 |
| 장애 타입 | QueueOverflow |
| 장애 원인 | Queueload is too high |
| 장애 설명 | Queueload is A% (CRI:B,MAJ:C,MIN:D) |

A : 현재 Queue사용률

B : CRITICAL 등급 임계치

C : MAJOR 등급 임계치

D : MINOR 등급 임계치


**조치사항**

2.1) 발생 위치를 확인하여 해당 Alarm 이 발생한 장비에서 Queue 사용률을 확인한다.

2.2) Queue 부하가 많이 발생한 프로세스의 로그(~/ibc/log/프로세스명/프로세스명.mmdd)를 확인한다.


### A0090


**메시지 설명**

과금 프로세스 혹은 연동 문제로 인해 과금 로그 파일이 정상적으로 기록되지 않을 때 발생

| 구분 | 내 용 |
|---|---|
| 장애 CODE | A0090 |
| 장애 등급 | CLEARED, CRITICAL |
| 발생 위치 | 시스템명/LB/CHARGE |
| 장애 타입 | CDRDataEmpty |
| 장애 원인 | CDR Data Not Exist |
| 장애 설명 | CDR Data Not Exist(hh:mm ~ hh:mm) |


**조치사항**

2.1) 장애가 발생한 장비에서 과금 프로세스 로그(~/ibc/log/프로세스명/)를 통해 원인을 확인한다.


## 제 3장  Fault 메시지


### F4001


**메시지 설명**

수신 받은 SIP request의 Max-Forwards 헤더가 없거나 값이 0일 경우

| 구분 | 내 용 |
|---|---|
| CODE | F4001 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | Session ID, SIP method와 response code, 수신 address 정보 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd)를 확인한다.


### F4002


**메시지 설명**

수신 받은 SIP의 syntax 에러일 경우

| 구분 | 내 용 |
|---|---|
| CODE | F4002 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | SIP 전체 메시지 및 Syntax Error에 대한 상세 정보를 표시함 |


**조치사항**

2.1) SIP 메시지를 확인한다


### F4006


**메시지 설명**

비동기 작업 중인 Session에 해당되는 수신 SIP 메시지 처리 시 WaitJob 수행 실패를 할 경우

| 구분 | 내 용 |
|---|---|
| CODE | F4006 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | SIP method와 response code, 수신 address 정보 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_core.mmdd)를 확인한다.


### F4009


**메시지 설명**

SIP를 수신 시 Dialog에서 early dialog 개수가 full된 경우

| 구분 | 내 용 |
|---|---|
| CODE | F4009 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | SIP method와 response code, 수신 address 정보 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd, ~/log/CCM/CCM_core.mmdd)를확인한다.


### F400A


**메시지 설명**

SIP를 수신 시 Dialog 개수가 full된 경우

| 구분 | 내 용 |
|---|---|
| CODE | F400A |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | 현재 Dialog 개수, 최대 Dialog 개수, SIP method와 response code, 수신 address 정보 |


**조치사항**

2.1) OMC를 통해 현재 Session 수를 확인


### F400B


**메시지 설명**

SIP를 수신 시 Dialog를 찾지 못한 경우

| 구분 | 내 용 |
|---|---|
| CODE | F400B |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | SIP method와 response code, 수신 address 정보 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd, ~/log/CCM/CCM_core.mmdd)를확인한다.


### F400C


**메시지 설명**

FSM에 맞지 않는 SIP를 수신하는 경우

| 구분 | 내 용 |
|---|---|
| CODE | F400C |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | Dialog정보, 현재 FSM state, SIP method와 response code, 수신 address 정보 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd, ~/log/CCM/CCM_core.mmdd)를확인한다.


### F400E


**메시지 설명**

발신 대국의 상태가 unavail 상태일 경우

| 구분 | 내 용 |
|---|---|
| CODE | F400E |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | Route ID, SIP method와 response code, 수신 address 정보 |


**조치사항**

2.1) 발신 대국의 상태를 확인한다.


### F4039


**메시지 설명**

SIP C timer가 만료된 경우

| 구분 | 내 용 |
|---|---|
| CODE | F4039 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | Dialog 정보, Expire 시간 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd)를 확인한다.


### F403C


**메시지 설명**

Alive timer가 만료된 경우

| 구분 | 내 용 |
|---|---|
| CODE | F403C |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | Dialog 정보, Expire 시간 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd)를 확인한다.


### F403D


**메시지 설명**

Garbage timer가 만료된 경우

| 구분 | 내 용 |
|---|---|
| CODE | F403D |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | Dialog 정보, Expire 시간 |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd)를 확인한다.


### F403E


**메시지 설명**

송신 SIP request에 대한 timer가 만료된 경우

| 구분 | 내 용 |
|---|---|
| CODE | F403E |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CM/CCM |
| 로그 | Dialog 정보, Expire 시간, SIP method와 response code |


**조치사항**

2.1) 프로세스의 로그(~/log/CCM/CCM_dump.mmdd)를 확인한다.


### F4100


**메시지 설명**

호가 인입 되었는데, 해당 호가 대국정보에 없을 시

| 구분 | 내 용 |
|---|---|
| CODE | F4100 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 대국 설정 정보를 살펴 본다.


### F4101


**메시지 설명**

호가 인입 되었는데, 사대 대국이 비가용 상태 일 때

| 구분 | 내 용 |
|---|---|
| CODE | F4101 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 대국 가용 상태를 확인 한다.


### F4102


**메시지 설명**

호가 인입 되었는데 대국 결정을 못할 때

| 구분 | 내 용 |
|---|---|
| CODE | F4102 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 라우팅 테이블이 설정을 확인 한다.


### F4200


**메시지 설명**

호가 인입 되었는데 필터 정책에 의해 거절 되었을 때

| 구분 | 내 용 |
|---|---|
| CODE | F4200 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 필터 설정을 확인 한다.


### F4300


**메시지 설명**

호가 인입 되었는데 TGAS 연결이 끊겨 호가 거절 되었을 때

| 구분 | 내 용 |
|---|---|
| CODE | F4300 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) CCM-TGAS 연결 상태를 확인 한다.


### F4301


**메시지 설명**

호가 인입 되었는데 TGAS가 비 가용 상태일 때

| 구분 | 내 용 |
|---|---|
| CODE | F4301 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) CCM-TGAS 연결 상태를 확인 한다.


### F4302


**메시지 설명**

호가 인입 되었는데 TGAS에서 응답이 없어 호가 종료 되었을 경우

| 구분 | 내 용 |
|---|---|
| CODE | F4302 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 해당 호의 TGAS 로그를 확인 한다.


### F4303


**메시지 설명**

호가 인입 되었는데 TGA가 비가용 상태 일 때

| 구분 | 내 용 |
|---|---|
| CODE | F4303 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) EMP 상태를 확인 한다.


### F4304


**메시지 설명**

호가 인입 되었는데 TGA가 내부 장애 발생시

| 구분 | 내 용 |
|---|---|
| CODE | F4303 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | CCM_0X/CM |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) EMP 상태를 확인 한다.


### F5001


**메시지 설명**

eMP장비에서 CM에서 전달받은 메시지의 세션을 찾지 못한 경우

| 구분 | 내 용 |
|---|---|
| CODE | F5001 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F5002


**메시지 설명**

TGAS 프로세서의 자원이 부족한 경우

| 구분 | 내 용 |
|---|---|
| CODE | F5002 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.2) eMP의 자원 수용 상태를 확인한다.


### F5003


**메시지 설명**

TGAS 프로세서에서 메시지 처리중 내부 에러가 발생한경우

(CM과의 세션 상태가 맞지 않거나 정의되지 않은 에러는 해당 에러코드로 정의함)

| 구분 | 내 용 |
|---|---|
| CODE | F5003 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F5004


**메시지 설명**

양 단말간의 코덱의 Nego 작업을 실패한경우

| 구분 | 내 용 |
|---|---|
| CODE | F5004 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.2) Fault Message의 Log를 통해 단말 코덱정보를 확인한다.


### F5005


**메시지 설명**

TGAS 프로세서 내에서 활성화된 세션이 아니라 판단할경우(Garbage처리)

| 구분 | 내 용 |
|---|---|
| CODE | F5005 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.2) 설정값(~/config/TGAS/AS_REFRESH.cfg의 GARBAGE부분)을 확인한다.


### F5006


**메시지 설명**

eMP에서 CM과의 세션 불일치 현상이 발생할 경우

(이미 사용중인 세션입니다.)

| 구분 | 내 용 |
|---|---|
| CODE | F5006 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F5007


**메시지 설명**

인입된 SDP 메시지가 규격에 맞지 않은 Syntax를 사용한 경우

| 구분 | 내 용 |
|---|---|
| CODE | F5007 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.2) 로그를 통하여 인입된 SDP 내용을 확인한다.


### F5008


**메시지 설명**

해당 세션에 송수신되는 미디어가 없습니다.

| 구분 | 내 용 |
|---|---|
| CODE | F5008 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.2) 설정값(~/config/TGAS/AS_REFRESH.cfg의 NETFAIL부분)을 확인한다.

- KILL값이 1일 경우 기능 ON, 0일경우 기능 OFF

2.3) 설정값(~/config/TGAS/AS_REFRESH.cfg의 TIMER부분)을 확인한다.

- ValidAliveTime값(초)의 시간 동안 미디어가 없을경우 기능동작함.


### F5009


**메시지 설명**

수용할 수 없는 Content Type입니다.

(SIP 메시지의 Content-Type헤더의 값이 application/sdp 가 아닌 경우)

| 구분 | 내 용 |
|---|---|
| CODE | F5009 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F500A


**메시지 설명**

TGAS프로세서에서 TGA프로세서와의 연동에 문제가 있을 경우

| 구분 | 내 용 |
|---|---|
| CODE | F500A |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F500B


**메시지 설명**

Early Media Session에서 문제가 발생한 경우

(Offer와 Answer의 방향 인식이 잘못된 경우)

| 구분 | 내 용 |
|---|---|
| CODE | F5001 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F500C


**메시지 설명**

TGAS에서 인입된 ADD 메시지의 Response를 전달하지 못하고 Timeout발생한 경우

| 구분 | 내 용 |
|---|---|
| CODE | F500C |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F500D


**메시지 설명**

TGAS에서 인입된 MOD 메시지의 Response를 전달하지 못하고 Timeout발생한 경우

| 구분 | 내 용 |
|---|---|
| CODE | F500D |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F500E


**메시지 설명**

TGAS에서 인입된 DEL 메시지의 Response를 전달하지 못하고 Timeout발생한 경우

| 구분 | 내 용 |
|---|---|
| CODE | F500E |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F500F


**메시지 설명**

Offer 메시지 이후에 Answer 메시지가 인입되지 않고 Timeout 발생한 경우

(INVITE 이후에 200OK 오지 않음 등)

| 구분 | 내 용 |
|---|---|
| CODE | F500F |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.


### F5011


**메시지 설명**

eMP에 할당된 프로세스 자원이 부족합니다.

(할당된 IP, Port로 모든 자원이 Full인 경우)

| 구분 | 내 용 |
|---|---|
| CODE | F5011 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.2) GUI, MMI 등에서 미디어 자원 할당 상태를 점검한다.


### F5012


**메시지 설명**

TRTE(미디어 그룹) 정보를 찾을 수 없습니다.

| 구분 | 내 용 |
|---|---|
| CODE | F5012 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.1) MMI에서 CM의 ROUTE조회 부분의 TRTE 값과 eMP의 Media Route부분이 정상적으로 연동되었는지 확인한다.


### F5013


**메시지 설명**

Media Board 정보를 찾을 수 없습니다.

| 구분 | 내 용 |
|---|---|
| CODE | F5013 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.2) MMI에서 Media Board 정보를 조회하여 값을 확인한다.


### F5014


**메시지 설명**

TGA 프로세서의 정보를 찾을 수 없습니다.

| 구분 | 내 용 |
|---|---|
| CODE | F5014 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.1) 프로세서의 Config(~/log/TGAS/TGAS.cfg) 에서 TGA 설정된 값을 확인한다.


### F5015


**메시지 설명**

eMP의 POOL(미디어 IP/Port할당) 정보를 찾을 수 없는 경우

| 구분 | 내 용 |
|---|---|
| CODE | F5015 |
| 장애 등급 | INFO / WARNING / FAULT |
| 발생 위치 | eMP0X_X/TGAS |
| 로그 | 인입된 세션의 세션정보 및 내용을 표시함. |


**조치사항**

2.1) 프로세서의 로그(~/log/TGAS/TGAS.mmdd) 를 확인한다.

2.1) MMI의 미디어 POOL 조회를 통해 자원할당 상태를 점검한다.


### S1003, S1007, S1008


**메시지 설명**

1분 통계 적재 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1003, S1007, S1008 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED WRITING 1MIN STATISTICS (A) |

A : 통계 종류

S1003 : 루트<CDR>

S1007 : RTT 과금

S1008 : RTT SIP


### S1101 ~ S1109


**메시지 설명**

5분 통계 적재 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1101 ~ S1109 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED WRITING 5MIN STATISTICS (A) |

A : 통계 종류

S1101 : 시스템

S1102 : 장애

S1103 : 루트<CDR>

S1104 : 호<SIP>

S1105 : 과금

S1106 : MEDIA

S1107 : RTT 과금

S1108 : RTT SIP

S1109 : SIP Reason


### S1201 ~ S1209


**메시지 설명**

시간별 통계 적재 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1201 ~ S1209 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED WRITING HOUR STATISTICS (A) |

A : 통계 종류

S1201 : 시스템

S1202 : 장애

S1203 : 루트<CDR>

S1204 : 호<SIP>

S1205 : 과금

S1206 : MEDIA

S1207 : RTT 과금

S1208 : RTT SIP

S1209 : SIP Reason


### S1301 ~ S1309


**메시지 설명**

일별 통계 적재 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1301 ~ S1309 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED WRITING DAY STATISTICS (A) |

A : 통계 종류

S1301 : 시스템

S1302 : 장애

S1303 : 루트<CDR>

S1304 : 호<SIP>

S1305 : 과금

S1306 : MEDIA

S1307 : RTT 과금

S1308 : RTT SIP

S1309 : SIP Reason


### S1401 ~ S1409


**메시지 설명**

주별 통계 적재 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1401 ~ S1409 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED WRITING WEEK STATISTICS (A) |

A : 통계 종류

S1401 : 시스템

S1402 : 장애

S1403 : 루트<CDR>

S1404 : 호<SIP>

S1405 : 과금

S1406 : MEDIA

S1407 : RTT 과금

S1408 : RTT SIP

S1409 : SIP Reason


### S1501 ~ S1509


**메시지 설명**

월별 통계 적재 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1501 ~ S1509 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED WRITING MONTH STATISTICS (A) |

A : 통계 종류

S1501 : 시스템

S1502 : 장애

S1503 : 루트<CDR>

S1504 : 호<SIP>

S1505 : 과금

S1506 : MEDIA

S1507 : RTT 과금

S1508 : RTT SIP

S1509 : SIP Reason


### S1601 ~ S1609


**메시지 설명**

연별 통계 적재 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1601 ~ S1609 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED WRITING YEAR STATISTICS (A) |

A : 통계 종류

S1601 : 시스템

S1602 : 장애

S1603 : 루트<CDR>

S1604 : 호<SIP>

S1605 : 과금

S1606 : MEDIA

S1607 : RTT 과금

S1608 : RTT SIP

S1609 : SIP Reason


### S1610


**메시지 설명**

장애 이력 삭제 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1610 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED DELETING ALARM DATA (장애이력) |


### S1611


**메시지 설명**

로그성 데이터 삭제 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1611 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | COMPLETED DELETING DATA (*참조1) |

*참조1

운영관리/운영자 관리/운영로그

운영관리/운영자 관리/접속로그

MMI로그

호추적

호추적 통계

FAULT 메시지 – FAULT

FAULT 메시지 – WARN

FAULT 메시지 – INFO

LIFECYCLE 이력

GM 연동 장애 이력


### S1901


**메시지 설명**

시스템 통계 요약 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1901 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | OMP01 : MAX_CPU = 7, CPU = 6, MAX_Memory = 21, Memory = 21 |


### S1902


**메시지 설명**

루트(CDR) 통계 요약 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1902 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | REQUEST = 0, SUCCESS = 0, FAIL = 0, USAGE = 0 (루트<CDR>) |


### S1903


**메시지 설명**

호(SIP) 통계 요약 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1903 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | 성공률 = 0, 소통률 = 0, 완료율 = 0 (호<SIP>) |


### S1904


**메시지 설명**

OPTION 장애 체크(SIP) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1904 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | NODE ID = 1111, OPTIONS RX = 0, TX = 0 (OPTIONS 장애 체크<SIP>) |


### S1905


**메시지 설명**

과금 통계 요약 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1905 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | IBC52, 성공률 = 70% |


### S1906


**메시지 설명**

IN RTT(SIP) 통계 요약 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1906 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | IBC52, IN RTT(SIP) = 200 |


### S1907


**메시지 설명**

OUT RTT(SIP) 통계 요약 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1907 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | IBC52, OUT RTT(SIP) = 200 |


### S1908


**메시지 설명**

RTT(과금) 통계 요약 알림

| 구분 | 내 용 |
|---|---|
| CODE | S1908 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | IBC52, CGName = imscg1, RTT = 200 |


### S2101


**메시지 설명**

패키지 백업 결과 (성공) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2101 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | FINISHED PKG BACKUP (CM01) RESULT = SUCCESS |


### S2102


**메시지 설명**

패키지 백업 결과 (실패) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2102 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | FINISHED PKG BACKUP (CM01) RESULT = FAIL |


### S2103


**메시지 설명**

패키지 백업 파일 전송 결과 (성공) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2103 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | SEND TO OMP PKG BACKUP (CM01) RESULT = SUCCESS |


### S2104


**메시지 설명**

패키지 백업 파일 전송 결과 (실패) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2104 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | SEND TO OMP PKG BACKUP (CM01) RESULT = FAIL |


### S2105


**메시지 설명**

자동 패키지 백업 시작 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2105 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | START PKG AUTO BACKUP (CM01) |


### S2106


**메시지 설명**

자동 패키지 백업 결과 (성공) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2106 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | FINISHED PKG AUTO BACKUP (CM01) RESULT = SUCCESS |


### S2107


**메시지 설명**

자동 패키지 백업 결과 (실패) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2107 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | FINISHED PKG AUTO BACKUP (CM01) RESULT = FAIL |


### S2108


**메시지 설명**

자동 패키지 백업 파일 전송 결과 (성공) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2108 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | SEND TO OMP PKG AUTO BACKUP (CM01) RESULT = SUCCESS |


### S2109


**메시지 설명**

자동 패키지 백업 파일 전송 결과 (실패) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2109 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | SEND TO OMP PKG AUTO BACKUP (CM01) RESULT = FAIL |


### S2201


**메시지 설명**

DB 백업 결과 (성공) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2201 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | FINISHED DB BACKUP RESULT = SUCCESS |


### S2202


**메시지 설명**

DB 백업 결과 (실패) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2202 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | FINISHED DB BACKUP RESULT = FAIL |


### S2203


**메시지 설명**

DB 복구 결과 (성공) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2203 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | FINISHED DB RESTORE RESULT = SUCCESS |


### S2204


**메시지 설명**

DB 복구 결과 (실패) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2204 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | FINISHED DB RESTORE RESULT = FAIL |


### S2205


**메시지 설명**

자동 DB 백업 시작 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2205 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | START DB AUTO BACKUP |


### S2206


**메시지 설명**

자동 DB 백업 결과 (성공) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2206 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | FINISHED DB AUTO BACKUP RESULT = SUCCESS |


### S2207


**메시지 설명**

자동 DB 백업 결과 (실패) 알림

| 구분 | 내 용 |
|---|---|
| CODE | S2207 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | FINISHED DB AUTO BACKUP RESULT = FAIL |


### S3001


**메시지 설명**

절체 성공 알림

| 구분 | 내 용 |
|---|---|
| CODE | S3001 |
| 장애 등급 | INFO |
| 발생 위치 | OMP |
| 로그 | HA DACT SUCCESS |


### S3002


**메시지 설명**

절체 실패 알림

| 구분 | 내 용 |
|---|---|
| CODE | S3002 |
| 장애 등급 | FAULT |
| 발생 위치 | OMP |
| 로그 | HA DACT FAIL |


### S3003


**메시지 설명**

이중화 상태 알림

| 구분 | 내 용 |
|---|---|
| CODE | S3003 |
| 장애 등급 | WARNING |
| 발생 위치 | OMP |
| 로그 | CM, ACTIVE SYSTEM CHANGED [SERVER01(S), SERVER02(A)] |


### S3004


**메시지 설명**

절체 알림 (CPU 과부하)

| 구분 | 내 용 |
|---|---|
| CODE | S3004 |
| 장애 등급 | WARNING |
| 발생 위치 | OMP |
| 로그 | SEND DACT MESSAGE CPU ( %d > %d ) Server = CM Server |


### S3005


**메시지 설명**

절체 알림 (메모리 과부하)

| 구분 | 내 용 |
|---|---|
| CODE | S3005 |
| 장애 등급 | WARNING |
| 발생 위치 | OMP |
| 로그 | SEND DACT MESSAGE MEM ( %d > %d ) Server = CM Server |
