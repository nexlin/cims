"""csc — 사이트 영역 경로 해석 단위 시험 (services/site_paths, site_directory_layout.md).

배포본은 OAM 실체화가 준 영역 키(ServiceLogging.Dir·Recording.Dir·State.Dir·Content.Dir)를 그대로 쓰고, 비어 있으면
단일 루트 레이아웃(ems/core/oam/src/services/paths.py 와 같은 규칙)으로 해석한다.

  python3 -m unittest tests.test_csc_site_paths
"""
from __future__ import annotations

import os
import sys
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

import services.site_paths as sp  # noqa: E402


class SitePathsTests(unittest.TestCase):
    def test_site_layout_flat_and_nested(self):
        flat = {"ServiceLogging.Dir": "/s/log", "Recording.Dir": "/s/recordings",
                "State.Dir": "/s/state", "Content.Dir": "/s/content"}
        nested = {"ServiceLogging": {"Dir": "/s/log"}, "Recording": {"Dir": "/s/recordings", "OamUrl": "https://x"},
                  "State": {"Dir": "/s/state"}, "Content": {"Dir": "/s/content"}}
        for cfg in (flat, nested):
            self.assertEqual(sp.log_dir(cfg), "/s/log")
            self.assertEqual(sp.sip_log_dir(cfg), "/s/log/sip")
            self.assertEqual(sp.recordings_dir(cfg), "/s/recordings")
            self.assertEqual(sp.state_dir(cfg), "/s/state")
            self.assertEqual(sp.content_dir(cfg), "/s/content")
            self.assertEqual(sp.mcdata_fd_dir(cfg), "/s/content/mcdata_fd")

    def test_single_root_layout(self):
        cfg = {"ServiceLogging": {"Dir": "/nas/service_log/"}}
        self.assertEqual(sp.log_dir(cfg), "/nas/service_log")
        self.assertEqual(sp.recordings_dir(cfg), "/nas/service_log")
        self.assertEqual(sp.state_dir(cfg), "/nas/service_log/state")
        self.assertEqual(sp.content_dir(cfg), "/nas/service_log")
        self.assertEqual(sp.mcdata_fd_dir(cfg), "/nas/service_log/mcdata_fd")

    def test_empty_log_is_node_local(self):
        self.assertTrue(sp.log_dir({}).endswith(os.path.join("runtime", "service_log")))

    def test_mcdata_fd_explicit_wins(self):
        cfg = {"McDataFd": {"Dir": "/nas/fd", "MaxBytes": 10}, "Content": {"Dir": "/s/content"}}
        self.assertEqual(sp.mcdata_fd_dir(cfg), "/nas/fd")


if __name__ == "__main__":
    unittest.main()
