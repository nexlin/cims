-- 유선 VoIP 가입 테이블 분리 — voip_subscriptions (docs/design/features/sip_service_model.md §2-9,
--   volte_supplementary_services.md §10.1). 가입 테이블 = 접속환경 kind: 유선 voip 회선이 volte_subscriptions 행
--   (service_ref 가 kind=voip 접속서비스)으로 공존하던 것을 자기 테이블로 옮긴다.
--
-- 적용 (재실행 안전 — 테이블 존재 시 CREATE no-op, 이관은 INSERT IGNORE + 이관된 행만 DELETE):
--   mysql cims < sql/migrate_voip_subscriptions.sql
--   kind=voip 접속서비스 name 이 'voip' 하나가 아니면 세션 변수로 준다(쉼표 구분, 기본 'voip'):
--   mysql cims -e "SET @voip_refs := 'voip,voip-b'; SOURCE sql/migrate_voip_subscriptions.sql"
--   (SQL 은 관리 store 의 access_services.jsonl 을 읽지 못하므로 kind=voip 레코드의 name 목록은 운영자가 준다.)
--
-- 전제: volte_subscriptions 가 최종 형상(ha1·auth_scheme·k_enc·opc_enc·sqn·amf·sip_transport·pickup_group)이어야 한다 —
--   아니면 중단(ERROR 1054). 옮기는 행의 번호·H(A1)·pickup_group·phone_group_members.user_id(FK 없음)는 그대로라 단말
--   재로그인이 필요 없다. voip_subscriptions 를 읽는 csp/csc/oam 릴리스와 같은 정지창에서 적용한다(구 코드는 이 테이블을
--   모르고, 신 코드는 테이블 부재를 프로브로 감지해 건너뛴다).
--
-- 주의: 2026-04-22 이전 `voip_subscriptions` 는 전화 가족 테이블의 옛 이름이었다(migrate_voip_to_volte.sql 로 rename).
--   그 스크립트는 volte_subscriptions 가 없을 때만 RENAME 하도록 막아 두었다 — 이 테이블을 되돌리지 않는다.

SET @db := DATABASE();
SET @voip_refs := COALESCE(@voip_refs, 'voip');

-- 1. 테이블 (cims_schema.sql 과 같은 DDL)
CREATE TABLE IF NOT EXISTS voip_subscriptions (
    id            VARCHAR(64)  NOT NULL COMMENT 'VoIP MSISDN (E.164)',
    user_id       INT          NOT NULL COMMENT 'users.id 참조 (개인 ID)',
    auth_id       VARCHAR(128) NOT NULL DEFAULT '' COMMENT 'SIP Digest 인증 ID (IMPI)',
    imsi          VARCHAR(64)           DEFAULT NULL COMMENT '유선 규약: 번호 숫자 (USIM 없음)',
    service_ref   VARCHAR(64)           DEFAULT NULL COMMENT 'access_services.name (kind=voip 필수)',
    ha1           CHAR(32)     NOT NULL DEFAULT '' COMMENT 'SIP Digest H(A1)=MD5(imsi@domain:realm:password) — 인증 자료 SoT',
    auth_scheme   ENUM('digest','aka') NOT NULL DEFAULT 'digest' COMMENT '인증 체계: digest=SIP Digest(ha1) / aka=IMS AKA(k/opc/sqn) — 유선은 digest',
    k_enc         VARCHAR(160) NOT NULL DEFAULT '' COMMENT 'AKA K (AuC.Kek 암호화 보관)',
    opc_enc       VARCHAR(160) NOT NULL DEFAULT '' COMMENT 'AKA OPc (AuC.Kek 암호화 보관)',
    sqn           BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'AKA SQN_HE (48-bit, CSC 단일 발급자)',
    amf           CHAR(4)      NOT NULL DEFAULT '8000' COMMENT 'AKA AMF hex4',
    sip_transport ENUM('UDP','TCP','TLS')  DEFAULT NULL COMMENT '채널 정책: TLS=서버 집행(비-TLS 요청 403) / UDP·TCP=프로비저닝 힌트 / NULL=ANY(단말 선택). 유선 권장 기본 TLS',
    dnd           TINYINT(1)   NOT NULL DEFAULT 0  COMMENT '착신거부',
    forward_id    VARCHAR(64)  NOT NULL DEFAULT '' COMMENT '착신전환 대상',
    pickup_group  VARCHAR(64)           DEFAULT NULL COMMENT '당겨받기 그룹 = phone_groups.id (전화 그룹 멤버십에서 CSC 가 파생, NULL=어떤 픽업·BLF 축에도 속하지 않음)',
    register_time DATETIME              DEFAULT NULL,
    logout_time   DATETIME              DEFAULT NULL,
    PRIMARY KEY (id),
    KEY idx_user_id (user_id),
    CONSTRAINT fk_voipsub_user FOREIGN KEY (user_id) REFERENCES users (id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='유선 VoIP 가입자 인증 정보';

-- 2. 원본 형상 검사 — volte_subscriptions 가 최종 형상이 아니면 이관 전에 중단
SET @missing := (SELECT COUNT(*)
                   FROM (SELECT 'ha1' AS c UNION ALL SELECT 'auth_scheme' UNION ALL SELECT 'k_enc' UNION ALL SELECT 'opc_enc'
                         UNION ALL SELECT 'sqn' UNION ALL SELECT 'amf' UNION ALL SELECT 'sip_transport' UNION ALL SELECT 'pickup_group') need
                  WHERE need.c NOT IN (SELECT COLUMN_NAME FROM information_schema.COLUMNS
                                        WHERE TABLE_SCHEMA = @db AND TABLE_NAME = 'volte_subscriptions'));
--   중단 방식: 없는 컬럼을 고르는 SELECT 로 ERROR 1054 를 내 스크립트를 세운다(컬럼 이름이 사유) — SIGNAL 은 PREPARE 안에서
--   서버 종류에 따라 허용되지 않아 쓰지 않는다. mysql CLI 는 --force 없이는 첫 오류에서 멈춘다.
SET @sql := IF(@missing = 0, 'SELECT ''volte_subscriptions shape ok'' AS note',
    'SELECT ERROR_volte_subscriptions_not_final_shape__apply_migrate_subscription_ha1_aka_transport_pickup_group_first FROM volte_subscriptions LIMIT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 3. 이관 — volte 행 중 service_ref 가 kind=voip 접속서비스(@voip_refs)인 것을 복사(이미 있는 번호는 건너뜀)
INSERT IGNORE INTO voip_subscriptions
    (id, user_id, auth_id, imsi, service_ref, ha1, auth_scheme, k_enc, opc_enc, sqn, amf,
     sip_transport, dnd, forward_id, pickup_group, register_time, logout_time)
SELECT id, user_id, auth_id, imsi, service_ref, ha1, auth_scheme, k_enc, opc_enc, sqn, amf,
       sip_transport, dnd, forward_id, pickup_group, register_time, logout_time
  FROM volte_subscriptions
 WHERE FIND_IN_SET(COALESCE(service_ref, ''), @voip_refs) > 0;
SELECT ROW_COUNT() AS moved_rows;

-- 4. 원본 삭제 — voip_subscriptions 에 들어간 행만 (복사되지 않은 행은 남겨 유실을 막는다)
DELETE FROM volte_subscriptions
 WHERE FIND_IN_SET(COALESCE(service_ref, ''), @voip_refs) > 0
   AND id IN (SELECT id FROM voip_subscriptions);

-- 5. 결과 — volte_rows_still_voip 는 0 이어야 한다
SELECT 'migrate_voip_subscriptions applied' AS status,
       @voip_refs AS voip_refs,
       (SELECT COUNT(*) FROM voip_subscriptions) AS voip_rows,
       (SELECT COUNT(*) FROM volte_subscriptions
         WHERE FIND_IN_SET(COALESCE(service_ref, ''), @voip_refs) > 0) AS volte_rows_still_voip;
