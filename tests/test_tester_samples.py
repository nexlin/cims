"""계측기 미디어 샘플 라이브러리 단위시험 (services.tester_samples + handlers.tester /samples — 서버 미기동).
Covers:
  - 동봉 목록(소스 트리 tester/worker/samples 폴백)·마스터/코덱 파일 경로·이름 검증
  - 등록: id·WAV 검증, 변환기(build/bin/cims-sample-conv 가 있을 때) 로 16 kHz 마스터 + 코덱 파일 + 목록, 같은 id 409, 동봉 id 409, 교체, 삭제
  - 워커 동기화 판정(presence — health.media.files_detail 이름+크기)·run 전 배포(가짜 워커 put_file)
  - 핸들러: GET /samples · POST(octet-stream 본문) · GET master.wav(X-File-Path) · DELETE(manager)
각 테스트는 tmpdir 로 Tester.DataDir 격리.
"""
import asyncio
import io
import json
import math
import os
import shutil
import struct
import sys
import tempfile
import unittest
import wave

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
_TESTER = os.path.join(_REPO, 'ems', 'tester', 'oam')
for _m in [m for m in list(sys.modules) if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_TESTER, 'src'))
sys.path.insert(1, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(2, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))
from httpsrv.handler import HandlerArgs  # noqa: E402

admin_auth = H = S = SM = None
_SECRET = 'unit-test-secret'
_CONV = os.path.join(_REPO, 'build', 'bin', 'cims-sample-conv')


def _token(role):
    import jwt, time  # vendor
    return jwt.encode({'sub': 'u', 'login_id': 'u', 'role': role, 'exp': int(time.time()) + 600}, _SECRET, algorithm='HS256')


def _call(method, path, role='monitor', body=None, query=None):
    headers = {'authorization': f'Bearer {_token(role)}'} if role else {}
    args = HandlerArgs(method=method, full_path=path, client_ip='127.0.0.1', client_port=1,
                       query_params=query or {}, headers=headers, body=body)
    return asyncio.run(H.handle_tester(args, {'config': _CFG}))


def _wav(seconds=1.0, rate=16000, channels=1, freq=440.0, amp=8000):
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(channels); w.setsampwidth(2); w.setframerate(rate)
        n = int(seconds * rate)
        frames = b''.join(struct.pack('<' + 'h' * channels, *([int(amp * math.sin(2 * math.pi * freq * i / rate))] * channels)) for i in range(n))
        w.writeframes(frames)
    return buf.getvalue()


_TMP = _CFG = None


def setUpModule():
    global _TMP, _CFG, admin_auth, H, S, SM
    from services import admin_auth as _aa
    from handlers import tester as _H
    from services import tester_store as _S
    from services import tester_samples as _SM
    admin_auth, H, S, SM = _aa, _H, _S, _SM
    _TMP = tempfile.mkdtemp(prefix='tester-samples-ut-')
    _CFG = {'CimsRuntimeDir': os.path.join(_TMP, 'runtime'), 'CimsAuth': {'JwtSecret': _SECRET},
            'Tester': {'DataDir': os.path.join(_TMP, 'data')}}
    admin_auth.init(_CFG)
    from services import file_store, lease
    lease.acquire(file_store.runtime_root(_CFG))
    H.init(_TESTER, _CFG)


def tearDownModule():
    shutil.rmtree(_TMP, ignore_errors=True)


class FakeWorker:
    def __init__(self, name, files):
        self.name, self.health, self.health_error = name, {'media': {'files_detail': [{'name': n, 'size': s} for n, s in files.items()]}}, None
        self.pushed = []

    def probe(self):
        return self.health

    def put_file(self, name, data):
        self.pushed.append((name, len(data)))

    def to_dict(self):
        return {'name': self.name}


class Library(unittest.TestCase):
    def test_bundled_listing_and_paths(self):
        rows = SM.list_samples()
        ids = {r['id'] for r in rows}
        self.assertIn('speech_act40_kr', ids)
        self.assertIn('ringback_kr', ids)
        r = next(r for r in rows if r['id'] == 'speech_act40_kr')
        self.assertEqual(r['source'], 'bundled')
        self.assertEqual(r['files']['amr-wb'], 'speech_act40_kr.amrwb')
        self.assertEqual(r['amr-wb-dtx'], 'speech_act40_kr_dtx.amrwb')
        self.assertIsNotNone(SM.file_path('speech_act40_kr', 'speech_act40_kr.wav'))
        self.assertIsNotNone(SM.file_path('speech_act40_kr', 'speech_act40_kr_dtx.amrwb'))
        self.assertIsNone(SM.file_path('speech_act40_kr', '../samples.json'))
        self.assertIsNone(SM.file_path('speech_act40_kr', 'ringback_kr.pcmu'))   # 다른 샘플의 파일은 이 id 로 못 받는다
        self.assertIn('speech_act40_kr.pcmu', SM.files_by_name())

    def test_register_validation(self):
        with self.assertRaises(SM.SampleError) as cm:
            SM.register('bad id', _wav())
        self.assertEqual(cm.exception.code, 'bad_id')
        with self.assertRaises(SM.SampleError) as cm:
            SM.register('x1', b'not a wav')
        self.assertEqual(cm.exception.code, 'bad_wav')
        with self.assertRaises(SM.SampleError) as cm:
            SM.register('ringback_kr', _wav())
        self.assertEqual(cm.exception.code, 'bundled_read_only')

    @unittest.skipUnless(os.path.isfile(_CONV), 'build/bin/cims-sample-conv 없음')
    def test_register_convert_delete(self):
        row = SM.register('ut_tone', _wav(seconds=1.0, rate=44100, channels=2), kind='tone', description='단위시험 톤', normalize=-20.0)
        self.assertEqual(row['source'], 'user')
        self.assertEqual(row['kind'], 'tone')
        self.assertAlmostEqual(row['p56_active_level_dbov'], -20.0, delta=0.6)
        self.assertEqual(set(row['files']), {'pcmu', 'pcma', 'g722', 'amr-wb'})
        self.assertEqual(row['amr-wb-dtx'], 'ut_tone_dtx.amrwb')
        ud = SM.user_dir()
        master = os.path.join(ud, 'ut_tone.wav')
        with wave.open(master, 'rb') as w:
            self.assertEqual((w.getframerate(), w.getnchannels(), w.getsampwidth()), (16000, 1, 2))
            self.assertEqual(w.getnframes() % 320, 0)
        self.assertEqual(os.path.getsize(os.path.join(ud, 'ut_tone.amrwb')) % 61, 0)
        self.assertEqual(os.path.getsize(os.path.join(ud, 'ut_tone.pcmu')) % 160, 0)
        with open(os.path.join(ud, 'ut_tone_dtx.amrwb'), 'rb') as f:
            self.assertEqual(f.read(9), b'#!AMR-WB\n')
        self.assertEqual(SM.get_sample('ut_tone')['source'], 'user')
        with self.assertRaises(SM.SampleError) as cm:
            SM.register('ut_tone', _wav())
        self.assertEqual(cm.exception.code, 'exists')
        row2 = SM.register('ut_tone', _wav(seconds=0.5), replace=True, kind='other')
        self.assertAlmostEqual(row2['duration_s'], 0.5, delta=0.05)
        SM.delete('ut_tone')
        self.assertIsNone(SM.get_sample('ut_tone'))
        self.assertFalse(os.path.exists(master))
        with self.assertRaises(SM.SampleError):
            SM.delete('ut_tone')

    def test_presence_and_sync(self):
        rows = [r for r in SM.list_samples() if r['id'] == 'ringback_kr']
        idx = SM.files_by_name()
        full = {n: os.path.getsize(idx[n]) for n in SM.sample_file_names(rows[0])}
        stale = dict(full); stale['ringback_kr.pcmu'] = 1
        ws = [FakeWorker('w_ok', full), FakeWorker('w_stale', stale), FakeWorker('w_none', {})]
        pres = SM.presence(ws, rows)['ringback_kr']
        self.assertEqual(pres, {'w_ok': 'ok', 'w_stale': 'partial', 'w_none': 'missing'})
        notes = []
        SM.sync_for_run(ws, {'ringback': {'pcmu': 'ringback_kr.pcmu', 'amr-wb': 'synthetic', 'pcma': 'not_in_library.pcma'}}, notes)
        self.assertEqual(ws[0].pushed, [])
        self.assertEqual([n for n, _ in ws[1].pushed], ['ringback_kr.pcmu'])
        self.assertEqual([n for n, _ in ws[2].pushed], ['ringback_kr.pcmu'])   # 라이브러리 밖 파일은 건너뛴다
        self.assertTrue(any('w_stale' in n for n in notes))
        res = SM.sync_workers(ws, ['ringback_kr'])
        self.assertEqual(sorted(res['w_none']['pushed']), sorted(SM.sample_file_names(rows[0])))


class Handler(unittest.TestCase):
    def test_list_and_master(self):
        r = _call('GET', '/api/v1/tester/samples')
        self.assertEqual(r.status, 200)
        ids = {s['id'] for s in r.body['samples']}
        self.assertIn('conv_p59_a', ids)
        r = _call('GET', '/api/v1/tester/samples/conv_p59_a')
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body['pair'], 'conv_p59_b')
        r = _call('GET', '/api/v1/tester/samples/conv_p59_a/master.wav')
        self.assertEqual(r.status, 200)
        self.assertTrue(r.headers['X-File-Path'].endswith('conv_p59_a.wav'))
        self.assertEqual(r.headers['Content-Type'], 'audio/wav')
        r = _call('GET', '/api/v1/tester/samples/conv_p59_a/files/conv_p59_a_dtx.amrwb')
        self.assertEqual(r.status, 200)
        r = _call('GET', '/api/v1/tester/samples/conv_p59_a/files/samples.json')
        self.assertEqual(r.status, 404)
        r = _call('GET', '/api/v1/tester/samples/nope')
        self.assertEqual(r.status, 404)

    def test_register_rbac_and_errors(self):
        r = _call('POST', '/api/v1/tester/samples', role='monitor', body=_wav(), query={'id': 'h1'})
        self.assertEqual(r.status, 403)
        r = _call('POST', '/api/v1/tester/samples', role='operator', body={'x': 1}, query={'id': 'h1'})
        self.assertEqual(r.status, 400)
        self.assertEqual(r.body['error'], 'wav_body_required')
        r = _call('POST', '/api/v1/tester/samples', role='operator', body=b'zzz', query={'id': 'h1'})
        self.assertEqual(r.body['error'], 'bad_wav')
        r = _call('DELETE', '/api/v1/tester/samples/ringback_kr', role='manager')
        self.assertEqual(r.status, 409)

    @unittest.skipUnless(os.path.isfile(_CONV), 'build/bin/cims-sample-conv 없음')
    def test_register_roundtrip(self):
        r = _call('POST', '/api/v1/tester/samples', role='operator', body=_wav(rate=8000), query={'id': 'h_up', 'kind': 'announcement', 'description': '업로드', 'dtx': '0'})
        self.assertEqual(r.status, 201, r.body)
        self.assertNotIn('amr-wb-dtx', r.body)
        self.assertEqual(r.body['description'], '업로드')
        r = _call('GET', '/api/v1/tester/samples/h_up/master.wav')
        self.assertEqual(r.status, 200)
        r = _call('DELETE', '/api/v1/tester/samples/h_up', role='operator')
        self.assertEqual(r.status, 403)
        r = _call('DELETE', '/api/v1/tester/samples/h_up', role='manager')
        self.assertEqual(r.status, 200)


if __name__ == '__main__':
    unittest.main()
