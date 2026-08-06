#include "PSyncRtpRecorder.h"
#include "PLog.h"
#include <sys/time.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

PSyncRtpRecorder::PSyncRtpRecorder(const std::string& baseDir, const std::string& type,
                                   const std::string& caller, const std::string& callee)
    : _baseDir(baseDir), _type(type), _caller(caller), _callee(callee)
{
    // mkdir -p
    std::string path = baseDir;
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') { path[i] = '\0'; mkdir(path.c_str(), 0755); path[i] = '/'; }
    }
    mkdir(path.c_str(), 0755);
}

PSyncRtpRecorder::~PSyncRtpRecorder() {
    abort();
}

// ═══════════════════════════════════════════════════════════════
//  트랙 관리
// ═══════════════════════════════════════════════════════════════

void PSyncRtpRecorder::addTrack(const std::string& prefix) {
    if (_tracks.find(prefix) != _tracks.end()) return;   // 멱등 — 열려 있는 파일을 잃지 않는다
    Track t;
    t.prefix = prefix;
    _tracks[prefix] = t;
    // 세그먼트 진행 중 추가된 트랙(늦게 합류한 멤버의 상향 슬롯)은 즉시 연다 — 파일을
    //   세그먼트 시작 시점에만 열면 그 뒤 합류한 멤버의 미디어가 통째로 유실된다.
    if (_active) _openTrack(_tracks[prefix]);
}

void PSyncRtpRecorder::setTrackPtCodec(const std::string& prefix, int pt, const std::string& codec) {
    if (pt > 0) _trackPtCodec[prefix] = {pt, codec};
    else        _trackPtCodec.erase(prefix);
}

void PSyncRtpRecorder::setTrackSpeaker(const std::string& prefix, const std::string& speakerId) {
    int64_t now = _nowUsec();
    auto& spans = _trackSpans[prefix];

    if (!spans.empty() && spans.back().endUsec == 0) {
        if (spans.back().id == speakerId) return;   // 같은 화자 재호출 — 멱등
        spans.back().endUsec = now;                 // 화자 교대/이탈 → 이전 구간 종료
    }
    if (speakerId.empty()) return;                  // 빈 값 = 구간 닫기만

    SpeakerSpan s;
    s.id = speakerId;
    // 세그먼트 시작 전 귀속(트랙 선등록 시점)은 세그먼트 시작으로 당긴다 — 음수 offset 방지.
    s.startUsec = (_active && now < _segStartUsec) ? _segStartUsec : now;
    spans.push_back(s);
}

// 트랙 prefix → kind/slot(PTT)/side(VoIP)
void PSyncRtpRecorder::_trackKind(const std::string& prefix, std::string& kind, int& slot,
                                  std::string& side) const {
    kind.clear(); slot = -1; side.clear();
    if (_type == "ptt") {
        if (prefix.compare(0, 5, "audio") == 0)      { kind = "audio"; slot = atoi(prefix.c_str() + 5); }
        else if (prefix.compare(0, 5, "video") == 0) { kind = "video"; slot = atoi(prefix.c_str() + 5); }
        return;
    }
    // VoIP: a/b = 음성 leg, va/vb = 영상 leg
    if (prefix == "a" || prefix == "b")        { kind = "audio"; side = prefix; }
    else if (prefix == "va" || prefix == "vb") { kind = "video"; side = prefix.substr(1); }
}

// ═══════════════════════════════════════════════════════════════
//  세그먼트 시작/종료
// ═══════════════════════════════════════════════════════════════

// VoIP/일반: _baseDir 에 직접 기록 (seq 외부 지정)
void PSyncRtpRecorder::startSegment(int seq, const std::string& speakerId,
                                    int priority, bool preempted, const std::string& preemptedFrom) {
    if (_active) finishSegment();
    _trackSpans.clear();          // 화자 귀속은 세그먼트 단위
    _curSegDir = _baseDir;
    _curIndexDir = _baseDir;
    _currentSeq = seq;
    _speakerId = speakerId;
    _priority = priority;
    _preempted = preempted;
    _preemptedFrom = preemptedFrom;
    _segStartUsec = _nowUsec();
    _segEndUsec = 0;
    _lastPktUsec = 0;
    _active = true;
    _openTracks();
}

// PTT: 시간버킷 + shard, seq 는 시간 단위 자체 관리
void PSyncRtpRecorder::startPttSegment(const std::string& speakerId,
                                       int priority, bool preempted, const std::string& preemptedFrom,
                                       int audioPt, const std::string& audioCodec) {
    if (_active) finishSegment();
    setTrackPtCodec("audio", audioPt, audioCodec);   // 화자 leg PT/코덱 (화자마다 다를 수 있음)
    _trackSpans.clear();                             // 슬롯 트랙 귀속은 세그먼트 단위

    std::string hourDir = _hourDirNow();
    if (hourDir != _curHourDir) {
        _curHourDir = hourDir;
        // 버킷 내 seq 이어받기 — _hourSeq 는 레코더 인스턴스 메모리라, 세션 재시작
        // (새 그룹/레코더)이 같은 시간버킷에 들어오면 1부터 다시 매겨 이전 세션의
        // seg_NNNN_* 파일을 덮어쓴다. append-only 인덱스(segments.jsonl)가 정본이므로
        // 거기 마지막 seq 뒤에 이어붙인다.
        _hourSeq = _lastIndexedSeq(hourDir);
    }
    int seq = ++_hourSeq;
    int shard = (seq - 1) / 100;          // 100 세그먼트 단위 shard
    char shardBuf[8];
    snprintf(shardBuf, sizeof(shardBuf), "%03d", shard);
    _curSegDir = _curHourDir + "/seg/" + shardBuf;
    _curIndexDir = _curHourDir;
    _mkdirP(_curSegDir);

    _currentSeq = seq;
    _speakerId = speakerId;
    _priority = priority;
    _preempted = preempted;
    _preemptedFrom = preemptedFrom;
    _segStartUsec = _nowUsec();
    _segEndUsec = 0;
    _lastPktUsec = 0;
    _active = true;
    _openTracks();
}

// 시간버킷 segments.jsonl 의 최대 seq — 없으면 0. _writeMeta 가 seq 를 항상 행 선두에
// 기록하므로("{\"seq\":N,...") 행 선두 매칭이면 화자 문자열 등 내용 오탐이 없다.
int PSyncRtpRecorder::_lastIndexedSeq(const std::string& hourDir) {
    FILE* f = fopen((hourDir + "/segments.jsonl").c_str(), "r");
    if (!f) return 0;
    int last = 0;
    char line[4096];
    bool lineStart = true;
    while (fgets(line, sizeof(line), f)) {
        if (lineStart && strncmp(line, "{\"seq\":", 7) == 0) {
            int v = atoi(line + 7);
            if (v > last) last = v;
        }
        // fgets 가 긴 행을 쪼개 읽어도 다음 조각은 행 선두가 아니다
        lineStart = (strchr(line, '\n') != nullptr);
    }
    fclose(f);
    return last;
}

void PSyncRtpRecorder::_openTracks() {
    for (auto& [prefix, t] : _tracks) {
        (void)prefix;
        _openTrack(t);
    }
}

void PSyncRtpRecorder::_openTrack(Track& t) {
    char seqBuf[16];
    snprintf(seqBuf, sizeof(seqBuf), "%04d", _currentSeq);
    t.fileName = std::string("seg_") + seqBuf + "_" + t.prefix + ".rtp";
    t.finalPath = _curSegDir + "/" + t.fileName;
    t.tmpPath = t.finalPath + ".recording";
    t.bytesWritten = 0;
    t.mediaPackets = 0;
    t.fp = fopen(t.tmpPath.c_str(), "wb");
    if (!t.fp) {
        LOG_ERROR("PSyncRtpRecorder", "Failed to open %s: %s", t.tmpPath.c_str(), strerror(errno));
    } else {
        LOG_INFO("RtpRecorder", "Recording started: %s", t.tmpPath.c_str());
    }
}

void PSyncRtpRecorder::finishSegment() {
    if (!_active) return;

    // 발언시간 = 실제 미디어 구간. 마지막 패킷 시각이 있으면 그것을 종료로 삼아,
    // RELEASE 유실/지연(예: 검증 마지막 발언자가 RELEASE 없이 호 종료)으로 세그먼트가
    // 뒤늦게 닫혀도 floor 점유 시간이 발언시간으로 부풀지 않게 한다.
    _segEndUsec = (_lastPktUsec > _segStartUsec) ? _lastPktUsec : _nowUsec();
    _active = false;

    // 열려 있는 화자 구간을 세그먼트 종료로 닫는다 (마지막 화자는 RELEASE 없이 끝날 수 있다).
    for (auto& [prefix, spans] : _trackSpans) {
        (void)prefix;
        if (!spans.empty() && spans.back().endUsec == 0) spans.back().endUsec = _segEndUsec;
    }

    // 모든 트랙 파일 닫기 + rename
    for (auto& [prefix, t] : _tracks) {
        _closeTrack(t);
    }

    // 통합 메타 기록
    _writeMeta();

    LOG_INFO("PSyncRtpRecorder", "Segment finished: seq=%d duration=%lldms",
             _currentSeq, (long long)((_segEndUsec - _segStartUsec) / 1000));
}

void PSyncRtpRecorder::abort() {
    if (!_active) return;
    _active = false;
    for (auto& [prefix, t] : _tracks) {
        _closeTrack(t);
    }
}

void PSyncRtpRecorder::_closeTrack(Track& t) {
    if (t.fp) {
        fclose(t.fp);
        t.fp = nullptr;
        if (t.mediaPackets > 0) {
            rename(t.tmpPath.c_str(), t.finalPath.c_str());
            LOG_INFO("RtpRecorder", "Recording stopped: %s", t.finalPath.c_str());
        } else {
            // 미디어 없는 트랙(빈 파일 또는 헤더-only keepalive 만 — 음성 호의 영상 포트 등)은
            // 남기지 않고 제거 — 콘솔이 음성 호를 영상 녹취로 오판하는 원인.
            ::unlink(t.tmpPath.c_str());
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  패킷 기록
// ═══════════════════════════════════════════════════════════════

void PSyncRtpRecorder::writePacket(const std::string& prefix, const char* pkt, int len) {
    if (!_active || len <= 0) return;

    auto it = _tracks.find(prefix);
    if (it == _tracks.end() || !it->second.fp) return;

    Track& t = it->second;

    // 형식: [uint32 pkt_len][int64 recv_usec][rtp_pkt]
    uint32_t pktLen = (uint32_t)len;
    int64_t recvUsec = _nowUsec();

    fwrite(&pktLen, sizeof(pktLen), 1, t.fp);
    fwrite(&recvUsec, sizeof(recvUsec), 1, t.fp);
    fwrite(pkt, 1, len, t.fp);
    t.bytesWritten += sizeof(pktLen) + sizeof(recvUsec) + len;
    // payload 있는 패킷만 미디어로 집계 — 헤더-only(keepalive)는 파일 보존/메타 판정에서 제외
    if (len > 12 && len > 12 + ((unsigned char)pkt[0] & 0x0F) * 4)
        t.mediaPackets++;
    _lastPktUsec = recvUsec;
}

// ═══════════════════════════════════════════════════════════════
//  메타 기록
// ═══════════════════════════════════════════════════════════════

void PSyncRtpRecorder::_writeMeta() {
    char startTs[48], endTs[48];
    _isoUsec(_segStartUsec, startTs, sizeof(startTs));
    _isoUsec(_segEndUsec, endTs, sizeof(endTs));
    int64_t durationMs = (_segEndUsec - _segStartUsec) / 1000;

    char seqBuf[16];
    snprintf(seqBuf, sizeof(seqBuf), "%04d", _currentSeq);

    // 미디어(payload 있는 패킷) 보유 확인 — keepalive-only 트랙은 영상 없음으로 판정
    bool hasVideo = false;
    for (auto& [prefix, t] : _tracks) {
        if (t.mediaPackets > 0 && (prefix.find('v') == 0 || prefix == "video")) {
            hasVideo = true;
            break;
        }
    }

    // 파일 참조는 인덱스 기준(window/base) 상대경로 — PTT 는 seg/{NNN}/ shard 포함
    std::string relDir;
    if (_curSegDir != _curIndexDir && _curSegDir.size() > _curIndexDir.size() + 1)
        relDir = _curSegDir.substr(_curIndexDir.size() + 1) + "/";

    // JSON 생성
    auto writeJson = [&](FILE* f) {
        fprintf(f, "{\"seq\":%d,\"type\":\"%s\"", _currentSeq, _type.c_str());

        if (_type == "ptt") {
            if (!_speakerId.empty())
                fprintf(f, ",\"speaker_id\":\"%s\"", _jsonEsc(_speakerId).c_str());
            if (_priority >= 0)
                fprintf(f, ",\"priority\":%d", _priority);
            if (_preempted) {
                fprintf(f, ",\"preempt\":true");
                if (!_preemptedFrom.empty())
                    fprintf(f, ",\"preempted_from\":\"%s\"", _jsonEsc(_preemptedFrom).c_str());
            }
        } else {
            if (!_caller.empty())
                fprintf(f, ",\"caller\":\"%s\"", _jsonEsc(_caller).c_str());
            if (!_callee.empty())
                fprintf(f, ",\"callee\":\"%s\"", _jsonEsc(_callee).c_str());
        }

        fprintf(f, ",\"start_time\":\"%s\",\"end_time\":\"%s\",\"duration_ms\":%lld",
                startTs, endTs, (long long)durationMs);

        // 트랙별 파일 참조 (데이터가 있는 트랙만)
        for (auto& [prefix, t] : _tracks) {
            if (t.mediaPackets <= 0) continue;   // 미디어 없는 트랙(keepalive-only 포함)은 참조 미기록

            // 키 결정: VoIP는 audio_file_a/b, video_file_a/b, PTT는 audio_file, video_file
            std::string key;
            if (_type == "ptt") {
                if (prefix == "audio") key = "audio_file";
                else if (prefix == "video") key = "video_file";
                else key = prefix + "_file";
            } else {
                // VoIP: a→audio_file_a, b→audio_file_b, va→video_file_a, vb→video_file_b
                if (prefix == "a" || prefix == "b")
                    key = "audio_file_" + prefix;
                else if (prefix == "va")
                    key = "video_file_a";
                else if (prefix == "vb")
                    key = "video_file_b";
                else
                    key = prefix + "_file";
            }
            fprintf(f, ",\"%s\":\"%s\"", key.c_str(), _jsonEsc(relDir + t.fileName).c_str());

            // 오디오 트랙 PT/코덱 메타 — 변환기의 PT 판별 근거 (미지정 leg 는 생략 → 변환기 자동감지)
            auto itPc = _trackPtCodec.find(prefix);
            if (itPc != _trackPtCodec.end() && (prefix == "audio" || prefix == "a" || prefix == "b")) {
                std::string ptKey = (prefix == "audio") ? "audio_pt" : ("audio_pt_" + prefix);
                std::string cdKey = (prefix == "audio") ? "audio_codec" : ("audio_codec_" + prefix);
                fprintf(f, ",\"%s\":%d", ptKey.c_str(), itPc->second.first);
                if (!itPc->second.second.empty())
                    fprintf(f, ",\"%s\":\"%s\"", cdKey.c_str(), _jsonEsc(itPc->second.second).c_str());
            }

            // 동시 발언(dual/multi-talker) 슬롯 트랙의 화자 귀속 — 대표 화자(speaker_id)와
            // 다른 트랙만 기록한다(슬롯 0 은 speaker_id 가 곧 그 트랙의 화자).
            // 트랙 안에서 화자가 교대한 경우 첫 화자만 실린다 — 정본은 tracks[].speakers[].
            auto itSp = _trackSpans.find(prefix);
            if (itSp != _trackSpans.end() && !itSp->second.empty() && itSp->second[0].id != _speakerId)
                fprintf(f, ",\"speaker_id_%s\":\"%s\"", prefix.c_str(), _jsonEsc(itSp->second[0].id).c_str());
        }

        // ── tracks[] — 트랙 메타 정본 ────────────────────────────────────
        // 위 flat 키(audio_file/audio_pt/speaker_id_*)는 기존 녹취와의 호환을 위해 남기고,
        // 슬롯/leg·화자 구간·코덱을 모두 담는 정본은 이 배열이다. 소비자(OAM 변환기·콘솔)는
        // tracks[] 가 있으면 그것을 쓰고, 없으면(구 녹취) flat 키에서 합성한다.
        fprintf(f, ",\"tracks\":[");
        bool firstTrack = true;
        for (auto& [prefix, t] : _tracks) {
            if (t.mediaPackets <= 0) continue;

            std::string kind, side;
            int slot = -1;
            _trackKind(prefix, kind, slot, side);

            fprintf(f, "%s{\"prefix\":\"%s\"", firstTrack ? "" : ",", _jsonEsc(prefix).c_str());
            firstTrack = false;
            if (!kind.empty()) fprintf(f, ",\"kind\":\"%s\"", kind.c_str());
            if (slot >= 0)     fprintf(f, ",\"slot\":%d", slot);
            if (!side.empty()) fprintf(f, ",\"side\":\"%s\"", side.c_str());
            fprintf(f, ",\"file\":\"%s\"", _jsonEsc(relDir + t.fileName).c_str());

            auto itPc = _trackPtCodec.find(prefix);
            if (itPc != _trackPtCodec.end() && kind == "audio") {
                fprintf(f, ",\"pt\":%d", itPc->second.first);
                if (!itPc->second.second.empty())
                    fprintf(f, ",\"codec\":\"%s\"", _jsonEsc(itPc->second.second).c_str());
            }

            // 화자 구간 — 트랙 시작(=세그먼트 시작) 기준 offset. 슬롯 재사용으로 화자가
            //   교대한 트랙은 원소가 2개 이상이다.
            auto itSpans = _trackSpans.find(prefix);
            if (itSpans != _trackSpans.end() && !itSpans->second.empty()) {
                fprintf(f, ",\"speakers\":[");
                bool firstSpan = true;
                for (const auto& sp : itSpans->second) {
                    int64_t end = sp.endUsec > 0 ? sp.endUsec : _segEndUsec;
                    int64_t off = (sp.startUsec - _segStartUsec) / 1000;
                    int64_t len = (end - sp.startUsec) / 1000;
                    if (off < 0) { len += off; off = 0; }
                    if (len < 0) len = 0;
                    fprintf(f, "%s{\"id\":\"%s\",\"offset_ms\":%lld,\"dur_ms\":%lld}",
                            firstSpan ? "" : ",", _jsonEsc(sp.id).c_str(),
                            (long long)off, (long long)len);
                    firstSpan = false;
                }
                fprintf(f, "]");
            }
            fprintf(f, "}");
        }
        fprintf(f, "]");

        fprintf(f, ",\"has_video\":%s}\n", hasVideo ? "true" : "false");
    };

    // seg_NNNN.json — shard 디렉터리에
    std::string metaTmp = _curSegDir + "/seg_" + seqBuf + ".json.tmp";
    std::string metaFinal = _curSegDir + "/seg_" + seqBuf + ".json";
    FILE* f = fopen(metaTmp.c_str(), "w");
    if (f) {
        writeJson(f);
        fclose(f);
        rename(metaTmp.c_str(), metaFinal.c_str());
    }

    // segments.jsonl (append) — VoIP=_baseDir, PTT=시간버킷
    f = fopen((_curIndexDir + "/segments.jsonl").c_str(), "a");
    if (f) {
        writeJson(f);
        fclose(f);
    }
}

std::string PSyncRtpRecorder::_hourDirNow() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "/%04d/%02d/%02d/%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour);
    return _baseDir + buf;
}

void PSyncRtpRecorder::_mkdirP(const std::string& path) {
    std::string p = path;
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i] == '/') { p[i] = '\0'; mkdir(p.c_str(), 0755); p[i] = '/'; }
    }
    mkdir(p.c_str(), 0755);
}

// ═══════════════════════════════════════════════════════════════
//  유틸리티
// ═══════════════════════════════════════════════════════════════

int64_t PSyncRtpRecorder::_nowUsec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

void PSyncRtpRecorder::_isoUsec(int64_t usec, char* buf, int bufLen) {
    time_t sec = (time_t)(usec / 1000000LL);
    int microsec = (int)(usec % 1000000LL);
    struct tm t;
    localtime_r(&sec, &t);
    snprintf(buf, bufLen, "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, microsec);
}

std::string PSyncRtpRecorder::_jsonEsc(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else r += (char)c;
    }
    return r;
}
