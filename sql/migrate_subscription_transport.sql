-- 가입자 단위 SIP 시그널링 transport (docs/design/features/sip_tls_signaling.md §7)
--  - 종전엔 transport 가 Provisioning.Services.{volte,ptt} 서비스 단위뿐이라, TLS 전환이
--    "그 서비스 전 단말 동시 전환" 밖에 없었다. 계정 하나씩 옮기며 관찰하려면 가입자 단위
--    override 가 필요하다.
--  - NULL(기본) = 서비스 설정값 사용. 값이 있으면 그 가입자만 해당 transport 로 프로비저닝된다.
--  - CSC /provisioning/me 가 이 값을 읽어 단말 프로파일의 sip.transport 로 내려보낸다.
--    TLS 로 해석되면 포트도 서비스의 tls_port 로 바뀐다(설정 없으면 기존 port 유지).
--  - 재실행 안전 (컬럼 존재 시 no-op).

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'volte_subscriptions'
      AND COLUMN_NAME = 'sip_transport');
SET @sql := IF(@col = 0,
    'ALTER TABLE volte_subscriptions ADD COLUMN sip_transport ENUM(''UDP'',''TCP'',''TLS'') DEFAULT NULL
        COMMENT ''가입자 단위 시그널링 transport override (NULL=서비스 설정 사용)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_subscriptions'
      AND COLUMN_NAME = 'sip_transport');
SET @sql := IF(@col = 0,
    'ALTER TABLE ptt_subscriptions ADD COLUMN sip_transport ENUM(''UDP'',''TCP'',''TLS'') DEFAULT NULL
        COMMENT ''가입자 단위 시그널링 transport override (NULL=서비스 설정 사용)''',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
