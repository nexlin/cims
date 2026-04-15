#include "PSyncRtpRecorder.h"
#include "PLog.h"
#include <sys/time.h>
#include <sys/stat.h>
#include <cstring>

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

void PSyncRtpRecorder::startSegment(int seq, const std::string& speakerId) {
    if (_active) finishSegment();

    _currentSeq = seq;
    _speakerId = speakerId;
    _segStartUsec = _nowUsec();
    _segEndUsec = 0;
    _active = true;

    char seqBuf[16];
    snprintf(seqBuf, sizeof(seqBuf), "%04d", seq);

    for (auto& [prefix, t] : _tracks) {
        t.fileName = std::string("seg_") + seqBuf + "_" + prefix + ".rtp";
        t.finalPath = _baseDir + "/" + t.fileName;
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
        rename(t.tmpPath.c_str(), t.finalPath.c_str());
        LOG_INFO("RtpRecorder", "Recording stopped: %s", t.finalPath.c_str());
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

    // JSON 생성
    auto writeJson = [&](FILE* f) {
        fprintf(f, "{\"seq\":%d,\"type\":\"%s\"", _currentSeq, _type.c_str());

        if (_type == "ptt") {
            if (!_speakerId.empty())
                fprintf(f, ",\"speaker_id\":\"%s\"", _jsonEsc(_speakerId).c_str());
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
            fprintf(f, ",\"%s\":\"%s\"", key.c_str(), _jsonEsc(t.fileName).c_str());
        }

        fprintf(f, ",\"has_video\":%s}\n", hasVideo ? "true" : "false");
    };

    // seg_NNNN.json
    std::string metaTmp = _baseDir + "/seg_" + seqBuf + ".json.tmp";
    std::string metaFinal = _baseDir + "/seg_" + seqBuf + ".json";
    FILE* f = fopen(metaTmp.c_str(), "w");
    if (f) {
        writeJson(f);
        fclose(f);
        rename(metaTmp.c_str(), metaFinal.c_str());
    }

    // segments.jsonl (append)
    f = fopen((_baseDir + "/segments.jsonl").c_str(), "a");
    if (f) {
        writeJson(f);
        fclose(f);
    }
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
