"""ems/core/oam — MCPTT 그룹 정보 화면의 서비스 표시(MCVideo) 단위시험 (오프라인, DB 없음).

mcptt_management_views.md §3 — 한 그룹 = 서비스 집합(TS 23.280 §3): MCPTT 는 늘, MCVideo 는 그룹 문서에 MCVideo 몫
(`mcvideo_group_attrs` 행)이 있을 때. 표가 없으면(sql/migrate_mcvideo.sql 전) 어느 그룹도 MCVideo 가 아니다.

  python3 tests/test_oam_ptt_group_mcvideo.py
"""
from __future__ import annotations

import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from handlers import stats  # noqa: E402


class _Cur:
    """execute 한 SQL 을 남기고, 표가 없으면 pymysql 처럼 예외를 낸다."""
    def __init__(self, rows=None, missing=False):
        self.rows, self.missing, self.sql = rows or [], missing, []

    def execute(self, sql, args=None):
        self.sql.append(sql)
        if self.missing:
            raise RuntimeError("(1146, \"Table 'cims.mcvideo_group_attrs' doesn't exist\")")

    def fetchall(self):
        return self.rows


class McVideoGroupIdsTests(unittest.TestCase):
    def test_rows_are_mcvideo_groups(self):
        cur = _Cur([{'mcptt_group_id': 'gmv1'}, {'mcptt_group_id': 'gmv2'}])
        self.assertEqual(stats._mcvideo_group_ids(cur), {'gmv1', 'gmv2'})
        self.assertIn('mcvideo_group_attrs', cur.sql[0])

    def test_one_group_filter(self):
        cur = _Cur([{'mcptt_group_id': 'gmv1'}])
        self.assertEqual(stats._mcvideo_group_ids(cur, 'gmv1'), {'gmv1'})
        self.assertIn('WHERE g.mcptt_group_id=%s', cur.sql[0])

    def test_no_rows_no_mcvideo(self):
        self.assertEqual(stats._mcvideo_group_ids(_Cur([])), set())

    def test_pre_migration_table_absent(self):
        self.assertEqual(stats._mcvideo_group_ids(_Cur(missing=True)), set())


if __name__ == "__main__":
    unittest.main()
