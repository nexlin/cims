-- IMS AKA 인증 자료 (docs/design/features/sip_access_security.md §8.2 — P3, TS 33.203 Annex X)
--  - auth_scheme : 가입자의 인증 체계. 'digest'(기본, SIP Digest — ha1) | 'aka'(IMS AKA — k/opc/sqn).
--                  Cx 모델의 SIP-Authentication-Scheme 상당 — CSP 는 이 값으로 챌린지 체계를 고른다
--                  (협상이 아니라 프로비저닝으로 확정, TS 33.203 Annex P.4).
--  - k_enc/opc_enc : 128-bit K / OPc 를 CSC 의 AuC.Kek 로 암호화한 보관 형식
--                  (v1:<iv16><ct16><hmac32> hex — csc/src/services/auc/keystore.py). 평문 K/OPc 는
--                  CSC 프로세스 밖으로 나가지 않는다(AV 만 나간다). 소프트-K 프로비저닝(/provisioning/me)
--                  은 예외 — 단말이 USIM 역할이므로 TLS+토큰 인증 채널로만 전달한다.
--  - sqn         : HE 측 SQN(48-bit, TS 33.102 Annex C). AV 발급마다 +1, AUTS 재동기 시 SQN_MS 로 교체.
--                  발급자는 CSC 하나(단일 쓰기 주체) — CSP 는 읽지도 않는다.
--  - amf         : AMF 16-bit hex4 (기본 8000 — TS 33.102 Annex H, separation bit 0).
--  - 재실행 안전 (컬럼 존재 시 no-op). ha1/sip_transport 와 같은 방식.

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'volte_subscriptions'
      AND COLUMN_NAME = 'auth_scheme');
SET @sql := IF(@col = 0,
    'ALTER TABLE volte_subscriptions
        ADD COLUMN auth_scheme ENUM(''digest'',''aka'') NOT NULL DEFAULT ''digest''
            COMMENT ''인증 체계: digest=SIP Digest(ha1) / aka=IMS AKA(k/opc/sqn) — CSP 챌린지 체계 선택'' AFTER ha1,
        ADD COLUMN k_enc   VARCHAR(160) NOT NULL DEFAULT '''' COMMENT ''AKA K (AuC.Kek 암호화 보관)'' AFTER auth_scheme,
        ADD COLUMN opc_enc VARCHAR(160) NOT NULL DEFAULT '''' COMMENT ''AKA OPc (AuC.Kek 암호화 보관)'' AFTER k_enc,
        ADD COLUMN sqn     BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''AKA SQN_HE (48-bit, CSC 단일 발급자)'' AFTER opc_enc,
        ADD COLUMN amf     CHAR(4) NOT NULL DEFAULT ''8000'' COMMENT ''AKA AMF hex4'' AFTER sqn',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_subscriptions'
      AND COLUMN_NAME = 'auth_scheme');
SET @sql := IF(@col = 0,
    'ALTER TABLE ptt_subscriptions
        ADD COLUMN auth_scheme ENUM(''digest'',''aka'') NOT NULL DEFAULT ''digest''
            COMMENT ''인증 체계: digest=SIP Digest(ha1) / aka=IMS AKA(k/opc/sqn) — CSP 챌린지 체계 선택'' AFTER ha1,
        ADD COLUMN k_enc   VARCHAR(160) NOT NULL DEFAULT '''' COMMENT ''AKA K (AuC.Kek 암호화 보관)'' AFTER auth_scheme,
        ADD COLUMN opc_enc VARCHAR(160) NOT NULL DEFAULT '''' COMMENT ''AKA OPc (AuC.Kek 암호화 보관)'' AFTER k_enc,
        ADD COLUMN sqn     BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''AKA SQN_HE (48-bit, CSC 단일 발급자)'' AFTER opc_enc,
        ADD COLUMN amf     CHAR(4) NOT NULL DEFAULT ''8000'' COMMENT ''AKA AMF hex4'' AFTER sqn',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
