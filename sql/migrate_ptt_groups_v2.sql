-- PTT 그룹 테이블 확장 마이그레이션
-- Usage: mysql -u cims -pcims1234 cims < sql/migrate_ptt_groups_v2.sql

-- 컬럼 존재 여부 확인 후 추가 (IF NOT EXISTS 미지원이므로 IGNORE)

ALTER TABLE ptt_groups
    ADD COLUMN session_start   DATETIME     DEFAULT NULL  COMMENT '그룹 세션 시작시간'
;
ALTER TABLE ptt_groups
    ADD COLUMN session_end     DATETIME     DEFAULT NULL  COMMENT '그룹 세션 종료시간 (NULL=무기한)'
;
ALTER TABLE ptt_groups
    ADD COLUMN priority        INT          DEFAULT 5     COMMENT '그룹 우선순위 (1=최고, 10=최저)'
;
ALTER TABLE ptt_groups
    ADD COLUMN encryption      TINYINT(1)   DEFAULT 0     COMMENT '암호화 여부'
;
ALTER TABLE ptt_groups
    ADD COLUMN emergency_call  TINYINT(1)   DEFAULT 0     COMMENT '긴급통화 허용'
;
ALTER TABLE ptt_groups
    ADD COLUMN org_code        VARCHAR(32)  DEFAULT NULL  COMMENT '소속 조직 코드'
;
