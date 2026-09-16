"""계측기 계약 모델 단위시험 (ems/tester/oam/src/services/tester_models.py).

Covers:
  - 패키지 동봉 샘플(토폴로지·시나리오·프로파일) 전부 검증 통과
  - extra='forbid' — 오타 키·미지 지표 이름·미정의 역할 참조 거절
  - 프로파일 모델별 필수 필드
  - schema/*.json 이 모델과 동기화(bin/gen-schemas 산출과 동일)
"""
import glob
import json
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
_TESTER = os.path.join(_REPO, 'ems', 'tester', 'oam')
for _m in [m for m in list(sys.modules) if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_TESTER, 'src'))
sys.path.insert(1, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(2, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

import yaml  # noqa: E402
from services.tester_models import SCHEMAS, schema_json, validate  # noqa: E402


def _load(p):
    with open(p, 'r', encoding='utf-8') as f:
        return yaml.safe_load(f)


class BundledSamples(unittest.TestCase):
    def test_topology_sample(self):
        m, errs = validate('topology', _load(os.path.join(_TESTER, 'scenarios', 'topology.sample.yaml')))
        self.assertEqual(errs, [])
        self.assertEqual(m.pools['pbx_hq'].profile, 'pbx')
        self.assertEqual(m.pools['pbx_hq'].trunk_register.user, 'pbx-hq')   # YAML 키 register → alias

    def test_scenarios(self):
        files = [p for p in glob.glob(os.path.join(_TESTER, 'scenarios', '*', '*.yaml')) if '/profiles/' not in p]
        self.assertTrue(files)
        for p in files:
            m, errs = validate('scenario', _load(p))
            self.assertEqual(errs, [], p)
            self.assertTrue(m.flow)

    def test_profiles(self):
        files = glob.glob(os.path.join(_TESTER, 'scenarios', 'profiles', '*.yaml'))
        self.assertTrue(files)
        for p in files:
            _, errs = validate('profile', _load(p))
            self.assertEqual(errs, [], p)


class Strictness(unittest.TestCase):
    def _scn(self, **over):
        doc = {'id': 'T-BASIC', 'roles': {'a': {'pool': 'p'}, 'b': {'pool': 'p', 'disjoint_from': 'a'}},
               'flow': [{'step': 'register', 'who': ['a', 'b'], 'expect': {'code': 200}},
                        {'step': 'invite', 'from': 'a', 'to': 'b'},
                        {'step': 'bye', 'from': 'a', 'expect': {'sdd_ms': {'p95': 300}}}]}
        doc.update(over)
        return doc

    def test_typo_key_rejected(self):
        _, errs = validate('scenario', self._scn(flows=[]))
        self.assertTrue(any('flows' in e for e in errs))

    def test_unknown_metric_rejected(self):
        doc = self._scn()
        doc['flow'][0]['expect'] = {'rrd': {'p95': 1}}
        _, errs = validate('scenario', doc)
        self.assertTrue(any('알 수 없는 지표' in e for e in errs))

    def test_undefined_role_rejected(self):
        doc = self._scn()
        doc['flow'][1]['to'] = 'ghost'
        _, errs = validate('scenario', doc)
        self.assertTrue(any('ghost' in e for e in errs))

    def test_step_actor_required(self):
        doc = self._scn()
        doc['flow'].append({'step': 'answer'})
        _, errs = validate('scenario', doc)
        self.assertTrue(any('answer' in e for e in errs))

    def test_profile_required_by_model(self):
        _, errs = validate('profile', {'model': 'step', 'start': 5})
        self.assertTrue(any('step' in e and 'hold_s' in e for e in errs))
        m, errs = validate('profile', {'model': 'constant', 'rate': 4, 'duration_s': 60})
        self.assertEqual(errs, [])
        self.assertEqual(m.unit, 'saps')

    def test_unknown_kind(self):
        m, errs = validate('nope', {})
        self.assertIsNone(m)
        self.assertTrue(errs)


class SchemaFilesInSync(unittest.TestCase):
    def test_schema_dir_matches_models(self):
        d = os.path.join(_TESTER, 'schema')
        for name in SCHEMAS:
            p = os.path.join(d, f'{name}.schema.json')
            self.assertTrue(os.path.isfile(p), f'{p} 없음 — bin/gen-schemas 실행')
            with open(p, 'r', encoding='utf-8') as f:
                on_disk = json.load(f)
            self.assertEqual(on_disk, schema_json(name), f'{name}: 모델과 스키마 파일 불일치 — bin/gen-schemas 재실행')
        extra = {os.path.basename(p)[:-len('.schema.json')] for p in glob.glob(os.path.join(d, '*.schema.json'))} - set(SCHEMAS)
        self.assertEqual(extra, set(), '모델 없는 스키마 파일')


if __name__ == '__main__':
    unittest.main()
