-- 설정 그룹 암시적 제휴 — docs/design/features/mcptt_standard_conformance.md §C9
--  (TS 24.379 §7.3.2 13) · §9.2.2.2.15 · TS 24.484 §8.3.2 user profile <OnNetwork><ImplicitAffiliations>).
--  ptt_group_members.implicit_affiliation = 이 멤버의 user profile <ImplicitAffiliations> 에 이 그룹을 싣는다 —
--  CSP 가 PTT 서비스 인가(REGISTER) 성공 때 이 그룹에 제휴를 기록한다(등록 수명, 재등록마다 갱신).
--  재실행 안전(information_schema 확인). 구 코드는 신 컬럼을 읽지 않아 선행 적용 무해 — 새 CSC·CSP 는 이 컬럼을 SELECT 하므로
--  배포 전에 적용한다. 기본값 0 = 암시적 제휴 없음(적용 전 서버 동작과 같다 — 종전 CSC 가 소속 그룹 전부를
--  <ImplicitAffiliations> 로 알렸지만 CSP 는 그 제휴를 기록하지 않았다).

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'ptt_group_members' AND COLUMN_NAME = 'implicit_affiliation');
SET @sql := IF(@have = 0,
    'ALTER TABLE ptt_group_members
       ADD COLUMN implicit_affiliation TINYINT(1) NOT NULL DEFAULT 0
           COMMENT ''user profile <ImplicitAffiliations> 대상 (TS 24.484 §8.3.2) — PTT 서비스 인가(REGISTER) 때 참여 기능이 이 그룹에 제휴를 기록한다 (TS 24.379 §9.2.2.2.15)''
           AFTER on_network_required',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
