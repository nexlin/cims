#ifndef __PSYNC_RTP_RECORDER_H__
#define __PSYNC_RTP_RECORDER_H__

#include <string>
#include <map>
#include <cstdint>
#include <cstdio>

/**
 * 다중 트랙 동기 RTP 녹취기
 *
 * 여러 미디어 트랙(음성A/B, 영상A/B 등)을 하나의 세그먼트 단위로 관리.
 * startSegment/finishSegment으로 모든 트랙이 동시에 시작/종료되며,
 * 메타데이터(seg_NNNN.json, segments.jsonl)도 통합 기록.
 *
 * 파일명: seg_{seq:04d}_{trackPrefix}.rtp
 * 녹취 중: seg_{seq:04d}_{trackPrefix}.rtp.recording
 */
class PSyncRtpRecorder {
public:
    /**
     * @param baseDir    녹취 디렉터리
     * @param type       "voip" | "ptt" (메타 기록용)
     * @param caller     발신자 (VoIP용)
     * @param callee     착신자 (VoIP용)
     */
    PSyncRtpRecorder(const std::string& baseDir, const std::string& type,
                     const std::string& caller = "", const std::string& callee = "");
    ~PSyncRtpRecorder();

    /** 트랙 추가 (prefix: "a", "b", "va", "vb", "audio", "video" 등) */
    void addTrack(const std::string& prefix);

    /** 세그먼트 시작 — 등록된 모든 트랙 파일 열기 */
    void startSegment(int seq, const std::string& speakerId = "");

    /** 세그먼트 종료 — 모든 트랙 파일 닫기 + 메타 기록 */
    void finishSegment();

    /** 트랙별 패킷 기록 (세그먼트 활성 시에만 동작) */
    void writePacket(const std::string& prefix, const char* pkt, int len);

    /** 현재 세그먼트 활성 여부 */
    bool isActive() const { return _active; }

    /** 현재 세그먼트 seq */
    int getCurrentSeq() const { return _currentSeq; }

    /** 모든 활성 파일 종료 (메타 없이) — 비정상 종료 시 */
    void abort();

private:
    struct Track {
        std::string prefix;
        FILE* fp = nullptr;
        std::string fileName;     // 상대 파일명 (seg_0001_a.rtp)
        std::string tmpPath;      // 절대 경로 (.recording)
        std::string finalPath;    // 절대 경로
        int64_t bytesWritten = 0;
    };

    void _closeTrack(Track& t);
    void _writeMeta();
    static int64_t _nowUsec();
    static void _isoUsec(int64_t usec, char* buf, int bufLen);
    static std::string _jsonEsc(const std::string& s);

    std::string _baseDir;
    std::string _type;        // "voip" | "ptt"
    std::string _caller;
    std::string _callee;

    std::map<std::string, Track> _tracks;
    bool _active = false;
    int _currentSeq = 0;
    int64_t _segStartUsec = 0;
    int64_t _segEndUsec = 0;
    std::string _speakerId;
};

#endif // __PSYNC_RTP_RECORDER_H__
