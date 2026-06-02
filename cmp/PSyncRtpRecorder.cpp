#include "PSyncRtpRecorder.h"
#include "PLog.h"
#include <sys/time.h>
#include <sys/stat.h>
#include <cstring>
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
    Track t;
    t.prefix = prefix;
    _tracks[prefix] = t;
}

// ═══════════════════════════════════════════════════════════════
//  세그먼트 시작/종료
// ═══════════════════════════════════════════════════════════════

// VoIP/일반: _baseDir 에 직접 기록 (seq 외부 지정)
void PSyncRtpRecorder::startSegment(int seq, const std::string& speakerId,
                                    int priority, bool preempted, const std::string& preemptedFrom) {
    if (_active) finishSegment();
    _curSegDir = _baseDir;
    _curIndexDir = _baseDir;
    _currentSeq = seq;
    _speakerId = speakerId;
    _priority = priority;
    _preempted = preempted;
    _preemptedFrom = preemptedFrom;
    _segStartUsec = _nowUsec();
    _segEndUsec = 0;
    _active = true;
    _openTracks();
}

// PTT: 시간버킷 + shard, seq 는 시간 단위 자체 관리
void PSyncRtpRecorder::startPttSegment(const std::string& speakerId,
                                       int priority, bool preempted, const std::string& preemptedFrom) {
    if (_active) finishSegment();

    std::string hourDir = _hourDirNow();
    if (hourDir != _curHourDir) {
        _curHourDir = hourDir;
        _hourSeq = 0;
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
    _active = true;
    _openTracks();
}

void PSyncRtpRecorder::_openTracks() {
    char seqBuf[16];
    snprintf(seqBuf, sizeof(seqBuf), "%04d", _currentSeq);
    for (auto& [prefix, t] : _tracks) {
        t.fileName = std::string("seg_") + seqBuf + "_" + prefix + ".rtp";
        t.finalPath = _curSegDir + "/" + t.fileName;
        t.tmpPath = t.finalPath + ".recording";
        t.bytesWritten = 0;
        t.fp = fopen(t.tmpPath.c_str(), "wb");
        if (!t.fp) {
            LOG_ERROR("PSyncRtpRecorder", "Failed to open %s: %s", t.tmpPath.c_str(), strerror(errno));
        } else {
            LOG_INFO("RtpRecorder", "Recording started: %s", t.tmpPath.c_str());
        }
    }
}

void PSyncRtpRecorder::finishSegment() {
    if (!_active) return;

    _segEndUsec = _nowUsec();
    _active = false;

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
        if (t.bytesWritten > 0) {
            rename(t.tmpPath.c_str(), t.finalPath.c_str());
            LOG_INFO("RtpRecorder", "Recording stopped: %s", t.finalPath.c_str());
        } else {
            // 데이터 없는 트랙(예: 음성 그룹의 video)은 빈 파일 남기지 않고 제거
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

    // 파일 존재 + 크기 확인
    bool hasVideo = false;
    for (auto& [prefix, t] : _tracks) {
        if (t.bytesWritten > 0 && (prefix.find('v') == 0 || prefix == "video")) {
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
            if (t.bytesWritten <= 0) continue;

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
        }

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
