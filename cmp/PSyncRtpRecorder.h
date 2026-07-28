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

    /** VoIP/일반 세그먼트 시작 — _baseDir 에 직접 기록 (seq 외부 지정). */
    void startSegment(int seq, const std::string& speakerId = "",
                      int priority = -1, bool preempted = false, const std::string& preemptedFrom = "");

    /** PTT 세그먼트 시작 — 현재 시각 시간버킷 {YYYY}/{MM}/{DD}/{HH}/seg/{NNN}/ 에 기록.
     *  seq 는 recorder 가 시간(HH) 단위로 자체 관리(시간 바뀌면 1부터 리셋),
     *  shard = (seq-1)/100 → seg/000(1~100), seg/001(101~200) …
     *  audioPt/audioCodec: 화자 leg 의 ingress audio PT·코덱(user_src_pt/user_codec) —
     *  세그먼트 메타(audio_pt/audio_codec)로 기록되어 변환기의 PT 판별 근거가 된다. */
    void startPttSegment(const std::string& speakerId,
                         int priority = -1, bool preempted = false, const std::string& preemptedFrom = "",
                         int audioPt = 0, const std::string& audioCodec = "");

    /** 트랙별 오디오 PT/코덱 메타 (VoIP leg 별 — remote_src_pt/remote_codec).
     *  pt<=0 이면 해당 트랙 메타 제거. 세그먼트 메타에 audio_pt_{prefix}/audio_codec_{prefix} 로 기록. */
    void setTrackPtCodec(const std::string& prefix, int pt, const std::string& codec);

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
        int64_t mediaPackets = 0; // payload 있는 RTP 수 — 헤더-only keepalive 만 기록된
                                  // 트랙(음성호의 영상 포트 등)을 미디어 있음으로 오판하지 않기 위함
    };

    void _closeTrack(Track& t);
    void _writeMeta();
    /** _baseDir 하위 현재 시각 시간버킷 {YYYY}/{MM}/{DD}/{HH} 경로 (mkdir 포함) */
    std::string _hourDirNow();
    /** mkdir -p (경로 내 모든 상위 디렉터리 생성) */
    static void _mkdirP(const std::string& path);
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
    int64_t _lastPktUsec = 0;      // 마지막으로 패킷을 쓴 시각 — 세그먼트 종료가 늦어도(RELEASE 유실)
                                   // 발언시간을 실제 미디어 구간으로 한정하기 위함
    std::string _speakerId;
    int _priority = -1;            // 화자 floor 우선순위
    bool _preempted = false;       // 선점으로 시작된 세그먼트
    std::string _preemptedFrom;    // 선점 직전 화자

    // 트랙별 오디오 PT/코덱 (prefix → {pt, codec}) — PTT="audio"(화자별 갱신), VoIP="a"/"b"(leg 별)
    std::map<std::string, std::pair<int, std::string>> _trackPtCodec;

    // 현재 세그먼트 출력 경로 (VoIP=_baseDir, PTT=shard 디렉터리)
    std::string _curSegDir;        // seg_*.rtp / seg_*.json 위치
    std::string _curIndexDir;      // segments.jsonl 위치 (VoIP=_baseDir, PTT=시간버킷)
    // PTT 시간버킷 + shard
    std::string _curHourDir;       // 현재 시간버킷 {base}/{YYYY}/{MM}/{DD}/{HH}
    int _hourSeq = 0;              // 현재 시간 내 세그먼트 시퀀스 (시간 바뀌면 리셋)

    void _openTracks();            // _curSegDir 에 등록 트랙 파일 열기 (공통)
};

#endif // __PSYNC_RTP_RECORDER_H__
