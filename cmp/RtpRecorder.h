#ifndef __RTP_RECORDER_H__
#define __RTP_RECORDER_H__

#include <string>
#include <cstdio>
#include <cstdint>
#include <mutex>

/**
 * @brief RTP 패킷을 파일로 덤프하는 녹취기 (CMP용)
 *
 * raw 파일 형식: [uint32_t len][RTP packet][uint32_t len][RTP packet]...
 *
 * CMP는 raw 저장만 담당. 트랜스코딩은 CSC에서 on-demand.
 */
class RtpRecorder {
public:
    RtpRecorder();
    ~RtpRecorder();

    bool Start(const std::string& rawPath);
    void WritePacket(const char* pkt, int len);
    void Stop();

    bool IsRecording() const { return _recording; }
    std::string GetRawPath() const { return _rawPath; }

private:
    FILE* _fp;
    bool _recording;
    std::string _rawPath;
    std::mutex _mutex;
};

#endif
