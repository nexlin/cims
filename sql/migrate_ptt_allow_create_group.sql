-- 사용자 MCPTT 프로파일 — PTT 그룹 생성 자격 (docs/design/features/mcptt_authorization.md §3)
--  allow_create_group: allow-create-group (CIMS 확장 — TS 24.484 에는 일반 그룹 생성용 요소가 없고
--  allow-regroup(임시 regroup)·allow-create-{group,user}-broadcast-group 만 있다. 일반 그룹의 생성·수정·삭제
--  인가는 규격상 GMS(TS 24.481) 측 "authorized user" 정책이라 프로파일 확장 요소로 둔다).
--  이 사용자가 GMS XCAP PUT 으로 새 그룹을 만들 자격. 만든 그룹의 수정·삭제는 소유
--  (ptt_groups.authorized_user_id) 로 판정하며 이 플래그와 무관. 관제사에게 OAM(콘솔·admin API)이 부여(기본 0).
--  재실행 안전(컬럼 존재 시 no-op). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_user_profile'
      AND COLUMN_NAME = 'allow_create_group');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_user_profile
       ADD COLUMN allow_create_group TINYINT(1) NOT NULL DEFAULT 0
           COMMENT ''allow-create-group (CIMS 확장, TS 24.484 ruleset 자리) — GMS XCAP 그룹 생성 자격 (관제사)''
           AFTER allow_ambient_listening',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
