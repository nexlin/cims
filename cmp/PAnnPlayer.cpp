#include "PAnnPlayer.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "PTranscoder.h"

static uint32_t _rnd32() {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(nullptr) ^ (unsigned)(uintptr_t)&seeded);
        seeded = true;
    }
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
}

PAnnPlayer::PAnnPlayer(const std::string& playId, int peerIdx, const std::string& codec, int pt, bool amrOctetAlign,
                       std::vector<Item> items, int repeat, int delayMs, int maxMs, int natWaitMs)
    : _playId(playId), _peerIdx(peerIdx & 1), _codec(codec), _pt(pt & 0x7F), _octetAlign(amrOctetAlign),
      _items(std::move(items)), _repeat(repeat < 0 ? 1 : repeat), _delayMs(delayMs < 0 ? 0 : delayMs),
      _maxMs(maxMs < 0 ? 0 : maxMs), _natWaitMs(natWaitMs < 0 ? 0 : natWaitMs), _repeatLeft(_repeat) {
    _ssrc = _rnd32();
    if (_ssrc == 0) _ssrc = 1;
    _seq = (uint16_t)_rnd32();
    _ts = _rnd32();
    if (!_items.empty()) _itemRepeatLeft = _items[0].repeat;
}

bool PAnnPlayer::ok(std::string& err) const {
    if (_items.empty()) {
        err = "media list empty";
        return false;
    }
    for (const Item& it : _items) {
        if (!it.media) {
            err = "media not found";
            return false;
        }
        if (!it.media->hasCodec(_codec)) {
            err = it.media->id + " has no " + _codec + " file";
            return false;
        }
        if (it.repeat == 0 && it.maxMs <= 0 && (_repeat != 0 || _items.size() > 1)) {
            // repeat 0 항목은 max_ms 가 있어야 다음 항목으로 넘어간다 — 단일 항목 무한 loop(hold) 만 예외
            err = it.media->id + " repeat 0 needs max_ms";
            return false;
        }
    }
    return true;
}

int PAnnPlayer::durationMs() const {
    if (_repeat == 0) return 0;
    long total = 0;
    for (const Item& it : _items) {
        long one = (long)it.media->frameCount(_codec) * kFrameMs;
        long len = it.repeat > 0 ? one * it.repeat : (long)it.maxMs;
        if (it.maxMs > 0 && len > it.maxMs) len = it.maxMs;
        total += len;
    }
    long all = total * _repeat + (long)_delayMs * (_repeat - 1);
    if (_maxMs > 0 && all > _maxMs) all = _maxMs;
    return (int)all;
}

int PAnnPlayer::playedMs(int64_t nowUs) const {
    (void)nowUs;
    return (int)(_elapsedFrames * kFrameMs);
}

std::string PAnnPlayer::mediaLabel() const {
    std::string s;
    for (const Item& it : _items) {
        if (!s.empty()) s += ",";
        s += it.media ? it.media->id : "?";
    }
    return s;
}

std::string PAnnPlayer::_makeRtp(const std::string& payload, bool marker) {
    std::string p(12, '\0');
    p[0] = (char)0x80;
    p[1] = (char)((marker ? 0x80 : 0) | _pt);
    p[2] = (char)(_seq >> 8);
    p[3] = (char)_seq;
    p[4] = (char)(_ts >> 24); p[5] = (char)(_ts >> 16); p[6] = (char)(_ts >> 8); p[7] = (char)_ts;
    p[8] = (char)(_ssrc >> 24); p[9] = (char)(_ssrc >> 16); p[10] = (char)(_ssrc >> 8); p[11] = (char)_ssrc;
    ++_seq;
    if (_codec == "AMR-WB") {
        // 저장 형식 프레임(ToC + 데이터) → RTP 페이로드(CMR 15 + ToC + 데이터, leg 의 octet-align)
        PTranscoder::AmrFrame f;
        unsigned char toc = (unsigned char)payload[0];
        f.ft = (toc >> 3) & 0x0F;
        f.q = (toc >> 2) & 0x01;
        f.bytes = (int)payload.size() - 1;
        if (f.bytes > (int)sizeof(f.data)) f.bytes = (int)sizeof(f.data);
        memcpy(f.data, payload.data() + 1, (size_t)f.bytes);
        return p + PTranscoder::buildAmrWb(f, _octetAlign);
    }
    return p + payload;
}

// 다음 프레임 위치로 — 항목 끝·항목 max_ms·시퀀스 끝·전체 repeat 을 처리한다.
bool PAnnPlayer::_advance() {
    const int step = PAnnCatalog::TsStep(_codec);
    _ts += (uint32_t)step;
    ++_elapsedFrames;
    _nextUs += (int64_t)kFrameMs * 1000;
    const long elapsedMs = _elapsedFrames * kFrameMs;

    if (_maxMs > 0 && elapsedMs >= _maxMs) {
        _done = true;
        _reason = "max";
        return false;
    }
    if (_delayFramesLeft > 0) return true;   // 무음 구간 진행 중

    const Item& it = _items[_item];
    const int n = it.media->frameCount(_codec);
    bool nextItem = false;
    if (++_frame >= n) {
        _frame = 0;
        if (it.repeat > 0 && --_itemRepeatLeft <= 0) nextItem = true;
    }
    if (it.maxMs > 0 && (elapsedMs - _itemStartMs) >= it.maxMs) nextItem = true;
    if (!nextItem) return true;

    ++_item;
    _frame = 0;
    _itemStartMs = elapsedMs;
    if (_item < (int)_items.size()) {
        _itemRepeatLeft = _items[_item].repeat;
        _markerNext = true;
        return true;
    }
    // 시퀀스 끝
    if (_repeat > 0 && --_repeatLeft <= 0) {
        _done = true;
        _reason = "completed";
        return false;
    }
    _item = 0;
    _itemRepeatLeft = _items[0].repeat;
    _delayFramesLeft = _delayMs / kFrameMs;
    _markerNext = true;
    return true;
}

bool PAnnPlayer::nextPayload(std::string& payload) {
    payload.clear();
    if (_done || _items.empty()) return false;
    _started = true;
    const int step = PAnnCatalog::TsStep(_codec);
    if (_delayFramesLeft > 0) {
        --_delayFramesLeft;
        _ts += (uint32_t)step;
        ++_elapsedFrames;
        if (_maxMs > 0 && _elapsedFrames * kFrameMs >= _maxMs) { _done = true; _reason = "max"; }
        return !_done;
    }
    const std::vector<std::string>& frames = _items[_item].media->frames.at(_codec);
    payload = frames[_frame];
    if (!payload.empty()) ++_sent;
    _advance();
    return true;
}

void PAnnPlayer::tick(int64_t nowUs, std::vector<std::string>& pkts) {
    if (_done || _items.empty()) return;
    if (!_ticked) { _ticked = true; _firstTickUs = nowUs; }
    if (!_started) {
        // NAT 게이트 — latch(첫 ingress) 또는 대기 상한까지 시작을 미룬다
        if (_natWaitMs > 0 && !_natOpen && (nowUs - _firstTickUs) < (int64_t)_natWaitMs * 1000) return;
        _started = true;
        _startUs = nowUs;
        _nextUs = nowUs;
        _itemStartMs = 0;
    }
    const int step = PAnnCatalog::TsStep(_codec);
    int burst = 0;
    while (!_done && nowUs >= _nextUs && burst < kMaxBurst) {
        if (_delayFramesLeft > 0) {
            // 반복 사이 무음 — 패킷 없이 시간·timestamp 만 진행(항목 프레임 위치는 건드리지 않는다)
            --_delayFramesLeft;
            _ts += (uint32_t)step;
            ++_elapsedFrames;
            _nextUs += (int64_t)kFrameMs * 1000;
            if (_delayFramesLeft == 0) _markerNext = true;
            if (_maxMs > 0 && _elapsedFrames * kFrameMs >= _maxMs) { _done = true; _reason = "max"; }
            continue;
        }
        const std::vector<std::string>& frames = _items[_item].media->frames.at(_codec);
        const std::string& payload = frames[_frame];
        if (!payload.empty()) {
            pkts.push_back(_makeRtp(payload, _markerNext));
            _markerNext = false;
            ++_sent;
            ++burst;
        } else {
            _markerNext = true;   // NO_DATA(무음) 뒤 첫 프레임은 새 talkspurt (RFC 3550 marker)
        }
        if (!_advance()) break;
    }
    // 크게 뒤처졌으면(리액터 지연) 마감을 현재로 끌어와 폭주를 막는다 — 재생 위치는 낸 프레임 수가 정한다
    if (!_done && nowUs - _nextUs > (int64_t)kMaxBurst * kFrameMs * 1000) _nextUs = nowUs;
}
