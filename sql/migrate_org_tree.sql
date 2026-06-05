-- 조직 트리 재구성: 회사 > 본부2 > 팀10(각 20명)
-- 기존 평면 10팀(TEAM01~10, parent NULL) → 회사/본부 상위 추가 + 팀 부모 지정.
-- users.org_id 는 팀 코드(TEAMxx) 유지(변경 없음). 멱등(code UNIQUE / parent 재지정).
-- Usage: sudo mysql cims < sql/migrate_org_tree.sql

-- 1) 회사(최상위)
INSERT INTO organizations (code, name, parent_id, sort_order)
  VALUES ('CORP', 'CIMS', NULL, 0)
  ON DUPLICATE KEY UPDATE name=VALUES(name), parent_id=NULL, sort_order=0;

-- 2) 본부 2개 (parent = 회사)
INSERT INTO organizations (code, name, parent_id, sort_order)
  VALUES ('DIV1', '제1본부', (SELECT id FROM (SELECT id FROM organizations WHERE code='CORP') t), 1)
  ON DUPLICATE KEY UPDATE name=VALUES(name),
    parent_id=(SELECT id FROM (SELECT id FROM organizations WHERE code='CORP') t), sort_order=1;
INSERT INTO organizations (code, name, parent_id, sort_order)
  VALUES ('DIV2', '제2본부', (SELECT id FROM (SELECT id FROM organizations WHERE code='CORP') t), 2)
  ON DUPLICATE KEY UPDATE name=VALUES(name),
    parent_id=(SELECT id FROM (SELECT id FROM organizations WHERE code='CORP') t), sort_order=2;

-- 3) 팀 부모 지정: 팀01~05 → 제1본부, 팀06~10 → 제2본부
UPDATE organizations SET parent_id=(SELECT id FROM (SELECT id FROM organizations WHERE code='DIV1') t)
  WHERE code IN ('TEAM01','TEAM02','TEAM03','TEAM04','TEAM05');
UPDATE organizations SET parent_id=(SELECT id FROM (SELECT id FROM organizations WHERE code='DIV2') t)
  WHERE code IN ('TEAM06','TEAM07','TEAM08','TEAM09','TEAM10');
