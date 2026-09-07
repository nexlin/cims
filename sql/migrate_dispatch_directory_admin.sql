-- 관제 그룹 — 조직/구성원/번호·PTT 그룹 관리 범위 (docs/design/features/dispatch_center.md §3.4)
--  directory_admin: 이 관제 그룹의 관제사(가입자, PKCE 토큰)가 관제 앱에서 조직 트리·구성원(person)·VoLTE/PTT 번호(가입)
--  ·PTT 그룹을 관리할 수 있는 범위. monitor_scope 와 같은 결의 범위 enum — 서버가 해석하고 앱은 결과만 받는다.
--    none = 관리 불가(기본)   own = 그룹 org_id 조직과 그 하위 조직   all = 전 조직
--  부여는 manager 이상(감청 범위와 같은 승인 사항). 3GPP 규격 밖(가입자 프로비저닝은 MC 서비스 제공자 정책)이라
--  CIMS 확장 컬럼으로 둔다. 재실행 안전(컬럼 존재 시 no-op). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해.

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dispatch_groups'
      AND COLUMN_NAME = 'directory_admin');
SET @sql := IF(@have = 0,
    'ALTER TABLE dispatch_groups
       ADD COLUMN directory_admin ENUM(''none'',''own'',''all'') NOT NULL DEFAULT ''none''
           COMMENT ''관제 앱 조직/구성원/번호·PTT 그룹 관리 범위 (own=org_id 하위)''
           AFTER listen_visibility',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
