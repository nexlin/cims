#!/usr/bin/env python3
"""OAM PTT 세션 이력 집계 — 동시 발언 반영 + private/ad-hoc 세션 노출 검증.

콘솔 좌측 목록의 출처는 DB(ptt_groups)가 아니라 녹취 디렉터리다. 그래야 DB 행이 없는
1:1 private call(priv-*)·ad-hoc 임시 그룹이 이력에 드러난다. 집계도 슬롯 트랙(동시 발언)을
반영해야 화자·발언 턴이 과소 집계되지 않는다.

실행: python3 tests/oam_ptt_history_test.py   (레포 루트에서)
"""
import os
import sys
import json
import shutil
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'ems/core/oam/src'))

import services.flow_logger as fl  # noqa: E402

PASS = FAIL = 0


def check(cond, msg):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok   {msg}")
    else:
        FAIL += 1
        print(f"  FAIL {msg}")


def _mk(root, key, descriptor, window, segs):
    base = os.path.join(root, 'ptt', key)
    wdir = os.path.join(base, *window)
    os.makedirs(wdir, exist_ok=True)
    if descriptor is not None:
        with open(os.path.join(base, 'group.json'), 'w') as f:
            json.dump(descriptor, f, ensure_ascii=False)
    with open(os.path.join(wdir, 'segments.jsonl'), 'w') as f:
        for s in segs:
            f.write(json.dumps(s, ensure_ascii=False) + '\n')


# 동시 발언 세그먼트: 슬롯 0 을 두 화자가 이어 쓰고(선점 회수), 슬롯 1·2 가 겹친다.
MULTI_SEG = {
    'seq': 1, 'type': 'ptt', 'speaker_id': '01011112222',
    'start_time': '2026-08-04T14:26:40.000000',
    'end_time': '2026-08-04T14:28:14.000000', 'duration_ms': 94000,
    'tracks': [
        {'prefix': 'audio', 'kind': 'audio', 'slot': 0, 'file': 'seg/000/a.rtp',
         'speakers': [{'id': '01011112222', 'offset_ms': 0, 'dur_ms': 58000},
                      {'id': '01099990000', 'offset_ms': 58000, 'dur_ms': 36000}]},
        {'prefix': 'audio1', 'kind': 'audio', 'slot': 1, 'file': 'seg/000/b.rtp',
         'speakers': [{'id': '01033334444', 'offset_ms': 25000, 'dur_ms': 44000}]},
        {'prefix': 'audio2', 'kind': 'audio', 'slot': 2, 'file': 'seg/000/c.rtp',
         'speakers': [{'id': '01055556666', 'offset_ms': 50000, 'dur_ms': 44000}]},
    ],
    'has_video': False,
}

LEGACY_SEG = {
    'seq': 1, 'type': 'ptt', 'speaker_id': '01077778888',
    'start_time': '2026-08-04T09:10:00.000000',
    'end_time': '2026-08-04T09:10:05.000000', 'duration_ms': 5000,
    'audio_file': 'seg/000/a.rtp', 'has_video': False,
}


def main():
    global PASS, FAIL
    tmp = tempfile.mkdtemp(prefix='cims_ptt_hist_')
    try:
        fl._calls_dir = tmp

        _mk(tmp, '7', {
            'id': 7, 'mcptt_group_id': 'g001', 'name': '1소대 지휘망',
            'group_type': 'prearranged', 'floor_control': 'on', 'floor_policy': 'multi',
            'max_talkers': 3, 'video_enabled': True, 'member_count': 6,
            'members': [{'user_id': '01011112222'}],
        }, ('2026', '08', '04', '14'), [MULTI_SEG])

        _mk(tmp, 'priv-01011112222-01055556666', {
            'id': 0, 'mcptt_group_id': 'priv-01011112222-01055556666',
            'name': 'private:01011112222-01055556666', 'group_type': 'private',
            'floor_control': 'off', 'floor_policy': 'single', 'max_talkers': 1,
            'member_count': 2,
            'members': [{'user_id': '01011112222'}, {'user_id': '01055556666'}],
        }, ('2026', '08', '04', '13'), [MULTI_SEG])

        _mk(tmp, 'adhoc-0912', {
            'id': 0, 'mcptt_group_id': 'adhoc-0912', 'name': 'adhoc:현장대응',
            'group_type': 'prearranged', 'member_count': 4, 'members': [],
        }, ('2026', '08', '04', '09'), [LEGACY_SEG])

        print("\n[1] 그룹 요약 — 분류(group/private/adhoc)와 세션 성격")
        sm = fl._ptt_group_summaries()
        check(set(sm) == {'7', 'priv-01011112222-01055556666', 'adhoc-0912'},
              f"녹취 디렉터리 3개 모두 노출 ({sorted(sm)})")
        check(sm['7']['kind'] == 'group', "DB 그룹(surrogate id>0) → group")
        check(sm['7']['floor_policy'] == 'multi' and sm['7']['max_talkers'] == 3,
              "그룹 floor 정책 동반")
        priv = sm['priv-01011112222-01055556666']
        check(priv['kind'] == 'private', "priv-* → private (DB 행 없이도 이력 노출)")
        check(priv['floor_control'] == 'off', "전이중 여부(floor_control=off) 동반")
        check(priv['peers'] == ['01011112222', '01055556666'], "1:1 상대 2인 추출")
        check(sm['adhoc-0912']['kind'] == 'adhoc', "surrogate id 없는 그룹 → adhoc")

        print("\n[2] 세션 집계 — 동시 발언 반영")
        s = fl._find_ptt_sessions('7')[0]
        check(s['segment_count'] == 1, f"세그먼트 1개 (실제 {s['segment_count']})")
        check(s['turn_count'] == 4, f"발언 턴 4건 — 슬롯0 재사용 포함 (실제 {s['turn_count']})")
        check(s['speaker_count'] == 4, f"화자 4명 — 슬롯 화자 전원 (실제 {s['speaker_count']})")
        check(s['max_concurrent'] == 3, f"최대 동시 발언 3명 (실제 {s['max_concurrent']})")
        check(s['total_speech_ms'] == 94000, "발화 구간 = 세그먼트 길이(겹침 1회)")
        check(s['talk_ms'] == 58000 + 36000 + 44000 + 44000, "발화 누적 = 화자별 합")

        print("\n[3] 구 녹취(flat 키) 하위호환")
        a = fl._find_ptt_sessions('adhoc-0912')[0]
        check(a['turn_count'] == 1, "턴 1건")
        check(a['speaker_count'] == 1, "화자 1명")
        check(a['max_concurrent'] == 1, "동시 발언 1명")

        print(f"\n결과: pass={PASS} fail={FAIL}")
        return 1 if FAIL else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
