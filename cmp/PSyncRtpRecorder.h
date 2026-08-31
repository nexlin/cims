#ifndef __PSYNC_RTP_RECORDER_H__
#define __PSYNC_RTP_RECORDER_H__

#include <atomic>
#include <string>
#include <map>
#include <memory>
#include <vector>
#include <cstdint>
#include <cstdio>

#include "StoreOpWriter.h"

// 녹취 저장 경로 연산 전담 worker (프로세스 공용) — RTP 리액터/제어 스레드는 녹취 경로
//   (RecordDir — NAS 가능)를 절대 만지지 않는다. 패킷/열기/닫기/메타 연산을 op 로 적재하면
//   worker 스레드 하나가 순서대로 실행한다. 저장 경로 실패/정체 시 패킷 op 드롭(장애 구간
//   녹취 유실 수용) + A-PRC-017 자기보고 — PCmpServer 가 startServer 에서 콜백을 구성한다.
extern CStoreOpWriter gclsRecStoreWriter;

/**
 * 다중 트랙 동기 RTP 녹취기
 *
 * 여러 미디어 트랙(음성A/B, 영상A/B 등)을 하나의 세그먼트 단위로 관리.
 * startSegment/finishSegment으로 모든 트랙이 동시에 시작/종료되며,
 * 메타데이터(seg_NNNN.json, segments.jsonl)도 통합 기록.
 *
 * 파일명: seg_{seq:04d}_{trackPrefix}.rtp
 * 녹취 중: seg_{seq:04d}_{trackPrefix}.rtp.recording
 *
 * 저장 경로 무의존 계약 — 호출 스레드(RTP 리액터·제어)는 트랙 상태/메타를 메모리에서만
 * 관리하고, 파일 I/O(디렉터리 생성·open/write/close·rename·메타/인덱스 기록·기존 seq
 * 계수)는 전부 gclsRecStoreWriter worker 가 수행한다. FILE* 는 worker 전용 테이블에
 * 산다. 세그먼트 rename 은 기존 파일을 덮지 않는다(.dup 우회) — 재기동 seq 시딩이
 * 비동기(늦으면 0부터)라도 기존 세그먼트가 파괴되지 않는 근거.
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

    /** PTT 세션 디렉터리 이름 (CSP 가 PTT_GROUP_ADD 의 session_dir 로 지정) — 시간버킷
     *  아래 한 겹 더. 기록 단위가 세션이므로 같은 시간대의 다음 통화와 섞이지 않는다.
     *  PTT 는 이 시점(제어 스레드)에 seq 시딩을 worker 로 예약한다. */
    void setSessionSubdir(const std::string& name);

    /** 트랙 추가 (prefix: "a", "b", "va", "vb", "audio", "video" 등) */
    void addTrack(const std::string& prefix);

    /** VoIP/일반 세그먼트 시작 — _baseDir 에 직접 기록 (seq 외부 지정). */
    void startSegment(int seq, const std::string& speakerId = "",
                      int priority = -1, bool preempted = false, const std::string& preemptedFrom = "");

    /** PTT 세그먼트 시작 — 현재 시각 시간버킷 {YYYY}/{MM}/{DD}/{HH}/{sesdir}/seg/{NNN}/ 에 기록.
     *  seq 는 **세션 단위 단조증가** — 세션이 시간버킷을 넘어가도 리셋하지 않는다(넘어간
     *  버킷의 segments.jsonl 과 seq 가 겹치지 않아야 이력 API 가 한 세션으로 이어붙일 수
     *  있다). 레코더가 새로 만들어진 경우(CMP 재기동 후 같은 세션 복귀)에만 그 디렉터리
     *  segments.jsonl 의 마지막 seq 를 이어받는다,
     *  shard = (seq-1)/100 → seg/000(1~100), seg/001(101~200) …
     *  audioPt/audioCodec: 화자 leg 의 ingress audio PT·코덱(user_src_pt/user_codec) —
     *  세그먼트 메타(audio_pt/audio_codec)로 기록되어 변환기의 PT 판별 근거가 된다. */
    void startPttSegment(const std::string& speakerId,
                         int priority = -1, bool preempted = false, const std::string& preemptedFrom = "",
                         int audioPt = 0, const std::string& audioCodec = "");

    /** 트랙별 오디오 PT/코덱 메타 (VoIP leg 별 — remote_src_pt/remote_codec).
     *  pt<=0 이면 해당 트랙 메타 제거. 세그먼트 메타에 audio_pt_{prefix}/audio_codec_{prefix} 로 기록. */
    void setTrackPtCodec(const std::string& prefix, int pt, const std::string& codec);

    /** 트랙별 화자 귀속 (PTT 동시 발언 — dual/multi-talker).
     *  한 슬롯 트랙은 세그먼트 도중 화자가 바뀔 수 있으므로(선점 회수 후 슬롯 재사용)
     *  귀속을 **구간(span) 목록**으로 누적한다 — 호출 시각을 경계로 이전 구간을 닫고
     *  새 구간을 연다. 같은 화자 재호출은 멱등, 빈 speakerId 는 현재 구간만 닫는다.
     *  세그먼트 메타에는 tracks[].speakers[] 로 기록된다. */
    void setTrackSpeaker(const std::string& prefix, const std::string& speakerId);

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
    /** 한 트랙 안에서 한 화자가 점유한 구간. endUsec=0 이면 아직 열려 있다. */
    struct SpeakerSpan {
        std::string id;
        int64_t startUsec = 0;
        int64_t endUsec = 0;
    };

    struct Track {
        std::string prefix;
        bool opened = false;      // open op 적재됨 (FILE* 는 worker 전용 테이블에 있다)
        std::string fileName;     // 상대 파일명 (seg_0001_a.rtp)
        std::string tmpPath;      // 절대 경로 (.recording)
        std::string finalPath;    // 절대 경로
        int64_t bytesWritten = 0;
        int64_t mediaPackets = 0; // payload 있는 RTP 수 — 헤더-only keepalive 만 기록된
                                  // 트랙(음성호의 영상 포트 등)을 미디어 있음으로 오판하지 않기 위함
    };

    void _closeTrack(Track& t);
    void _writeMeta();
    /** 트랙 prefix → 미디어 종류/슬롯(PTT)/leg(VoIP). tracks[] 메타 구성용.
     *  PTT: audio/audio1..N, video/video1..N (슬롯 번호). VoIP: a/b, va/vb (leg). */
    void _trackKind(const std::string& prefix, std::string& kind, int& slot, std::string& side) const;
    /** _baseDir 하위 현재 시각 시간버킷 {YYYY}/{MM}/{DD}/{HH} 경로 (순수 계산) */
    std::string _hourDirNow();
    /** 시간버킷 segments.jsonl 의 최대 seq (없으면 0) — 세션 재시작 시 이어받기용.
     *  저장 경로 읽기 — worker op 안에서만 호출한다 (시딩 예약: _enqueueSeed). */
    static int _lastIndexedSeq(const std::string& hourDir);
    /** 현재 시간버킷의 seq 시딩을 worker 로 예약 — 결과는 _seedSeq 에 담긴다.
     *  첫 startPttSegment 전에 도착하면 이어받고, 늦으면 0부터 (rename .dup 가드가 보호). */
    void _enqueueSeed();
    /** mkdir -p (경로 내 모든 상위 디렉터리 생성) — worker op 전용 */
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

    // 트랙별 오디오 PT/코덱 (prefix → {pt, codec}) — PTT=슬롯 트랙별(화자 leg), VoIP="a"/"b"(leg 별)
    std::map<std::string, std::pair<int, std::string>> _trackPtCodec;
    // 트랙별 화자 구간 (prefix → spans) — 슬롯 재사용으로 한 트랙에 화자가 연이어 기록되는
    //   경우까지 귀속한다. 세그먼트 단위로 초기화된다.
    std::map<std::string, std::vector<SpeakerSpan>> _trackSpans;

    // 현재 세그먼트 출력 경로 (VoIP=_baseDir, PTT=shard 디렉터리)
    std::string _curSegDir;        // seg_*.rtp / seg_*.json 위치
    std::string _curIndexDir;      // segments.jsonl 위치 (VoIP=_baseDir, PTT=시간버킷)
    // PTT 시간버킷 + shard
    std::string _sesSubdir;        // PTT 세션 디렉터리 이름 S{ts}_{n} (빈값=레거시 버킷 직행)
    std::string _curHourDir;       // 현재 기록 디렉터리 {base}/{YYYY}/{MM}/{DD}/{HH}[/{sesdir}]
    int _hourSeq = 0;              // 세션 내 세그먼트 시퀀스 (시간버킷을 넘어도 이어진다)
    // seq 시딩 결과 (-1=미도착) — worker 가 채우고 startPttSegment 가 합류. detach 대비 shared_ptr.
    std::shared_ptr<std::atomic<long long>> _seedSeq;
    bool _seedRequested = false;

    void _openTracks();            // _curSegDir 에 등록 트랙 파일 열기 (공통)
    void _openTrack(Track& t);     // 트랙 1개 열기 (세그먼트 중 추가된 트랙 포함)
};

#endif // __PSYNC_RTP_RECORDER_H__
