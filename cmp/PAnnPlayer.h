#ifndef __PANN_PLAYER_H__
#define __PANN_PLAYER_H__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "PAnnCatalog.h"

/**
 * 안내 재생기 — relay 세션의 peer leg 하나에 붙어 카탈로그 음원을 RTP 로 낸다 (announcements.md §4.2, cmp_media_api.md §6.7).
 *
 *   시퀀스 = 항목(음원 + repeat + max_ms) 열. 항목 repeat 0 = 그 항목을 max_ms 까지 반복(tone_then_announce 의 신호음 구간),
 *   전체 repeat 0 = STOP 까지 무한(hold·ringback). 반복 사이 delay_ms 는 무음(패킷 없음, timestamp 만 진행). 전체 max_ms 상한.
 *   RTP: 자기 SSRC·자기 seq, timestamp 는 코덱 클록(G.711/G.722 160, AMR-WB 320 per 20 ms), marker 는 시퀀스 시작·무음 뒤 첫 프레임.
 *   PT = leg 의 송신 PT. AMR-WB 페이로드는 leg fmtp 의 octet-align 으로 만든다(RFC 4867).
 *
 *   페이싱 — tick(now) 이 "지금까지 나가야 했던 프레임" 을 모두 낸다(늦은 틱은 최대 kMaxBurst 개까지 따라잡음). 시작은 첫 tick 시각.
 *   NAT 게이트 — natWaitMs > 0 이면 첫 ingress(onIngress) 또는 natWaitMs 경과까지 시작을 미룬다(latch 전 선언 주소 오송신 방지).
 *   스레드 규약: relay 리액터 안에서만 부른다(호출자가 relay _mutex 보유).
 */
class PAnnPlayer {
public:
    struct Item {
        std::shared_ptr<const PAnnMedia> media;
        int repeat = 1;   // 0 = maxMs 까지 반복
        int maxMs = 0;    // 0 = 제한 없음(repeat 가 정한다)
    };

    static const int kFrameMs = 20;
    static const int kMaxBurst = 5;

    PAnnPlayer(const std::string& playId, int peerIdx, const std::string& codec, int pt, bool amrOctetAlign,
               std::vector<Item> items, int repeat, int delayMs, int maxMs, int natWaitMs);

    /** 모든 항목이 이 코덱 파일을 갖는가 — 아니면 MEDIA_NOT_FOUND(err 에 첫 결손 id) */
    bool ok(std::string& err) const;

    /** 틱 — nowUs(단조 시각)까지 내야 할 RTP 패킷들을 pkts 에 채운다. 끝났으면 done() 이 참이 된다. */
    void tick(int64_t nowUs, std::vector<std::string>& pkts);
    /** ingress 관측(NAT 게이트 해제) */
    void onIngress() { _natOpen = true; }
    /** client 정지(STOP) / 교체(replaced) */
    void stop(const char* reason) { if (!_done) { _done = true; _reason = reason; } }
    /** mode=mix — 틱이 아니라 relay 패킷이 프레임을 당긴다(PAnnMixer). 다음 프레임 페이로드(저장 형식; 무음 구간은 빈 문자열).
     *  반환 false = 끝(done — completed/max). 재생 위치·played_ms 는 tick 과 같은 규칙으로 진행한다 */
    bool nextPayload(std::string& payload);
    void setMix(bool b) { _mix = b; }
    bool isMix() const { return _mix; }

    bool done() const { return _done; }
    const std::string& reason() const { return _reason; }
    int playedMs(int64_t nowUs) const;
    /** 시퀀스 1회 길이(ms) — repeat 0 항목은 maxMs, 전체 repeat 0 이면 0 */
    int durationMs() const;

    const std::string& playId() const { return _playId; }
    int peerIdx() const { return _peerIdx; }
    const std::string& codec() const { return _codec; }
    std::string mediaLabel() const;   // "sys:busy_kr,sys:ann_busy"
    uint32_t ssrc() const { return _ssrc; }
    long sent() const { return _sent; }

private:
    bool _advance();                   // 다음 프레임 위치로 — 끝이면 false(_done·_reason 설정)
    std::string _makeRtp(const std::string& payload, bool marker);

    std::string _playId;
    int _peerIdx;
    std::string _codec;
    int _pt;
    bool _octetAlign;
    std::vector<Item> _items;
    int _repeat;        // 전체 반복(0 = 무한)
    int _delayMs;
    int _maxMs;
    int _natWaitMs;

    // 진행 상태
    int _item = 0;
    int _frame = 0;
    int _itemRepeatLeft = 1;
    int64_t _itemStartMs = 0;     // 항목 시작(재생 시각 기준 ms) — 항목 max_ms 판정
    int _repeatLeft;
    int _delayFramesLeft = 0;     // 반복 사이 무음 프레임 수
    bool _markerNext = true;
    bool _done = false;
    std::string _reason;
    bool _mix = false;

    // 시간
    bool _ticked = false;         // 첫 tick 을 받았는가(게이트 대기 시작)
    bool _started = false;        // 실제 송출 시작 여부
    int64_t _firstTickUs = 0;     // 첫 tick(게이트 대기 포함)
    int64_t _startUs = 0;         // 실제 송출 시작
    int64_t _nextUs = 0;          // 다음 프레임 마감
    int64_t _elapsedFrames = 0;   // 낸(또는 무음으로 보낸) 프레임 수 = 재생 위치
    bool _natOpen = false;

    // RTP
    uint32_t _ssrc;
    uint16_t _seq;
    uint32_t _ts;
    long _sent = 0;
};

#endif  // __PANN_PLAYER_H__
