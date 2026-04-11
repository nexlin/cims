#include "RtpRecorder.h"
#include "CmpLog.h"
#include <cstring>
#include <sys/time.h>

RtpRecorder::RtpRecorder() : _fp(nullptr), _recording(false) {}

RtpRecorder::~RtpRecorder() { Stop(); }

bool RtpRecorder::Start(const std::string& rawPath) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_recording) return false;

    _fp = fopen(rawPath.c_str(), "wb");
    if (!_fp) {
        LOG_ERROR("RtpRecorder", "Failed to open %s: %s", rawPath.c_str(), strerror(errno));
        return false;
    }

    _rawPath = rawPath;
    _recording = true;
    LOG_INFO("RtpRecorder", "Recording started: %s", rawPath.c_str());
    return true;
}

void RtpRecorder::WritePacket(const char* pkt, int len) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_recording || !_fp || len <= 0) return;

    // 형식: [uint32 pkt_len][int64 recv_usec][rtp_pkt]
    uint32_t pktLen = (uint32_t)len;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t recvUsec = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;

    fwrite(&pktLen, sizeof(pktLen), 1, _fp);
    fwrite(&recvUsec, sizeof(recvUsec), 1, _fp);
    fwrite(pkt, 1, len, _fp);
}

void RtpRecorder::Stop() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_recording) return;

    if (_fp) {
        fclose(_fp);
        _fp = nullptr;
    }
    _recording = false;
    LOG_INFO("RtpRecorder", "Recording stopped: %s", _rawPath.c_str());
}
