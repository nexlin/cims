#ifndef _SIP_STATS_MONITOR_H_
#define _SIP_STATS_MONITOR_H_

#include <stdint.h>
#include <time.h>

#include <map>
#include <mutex>
#include <string>

#include "SipStackCounter.h"

class CMonitorString;

/**
 * @ingroup CspServer
 * @brief SIP 신호 통계 감시 — 스택 카운터(CSipStack::m_clsCounter)를 평가 윈도우
 *        (Setup.SipStats.EvalSec) 단위로 차분해 호/등록 성공률·신규 INVITE CPS·SIP 수신
 *        이상을 단계 임계로 평가하고 FM 자기보고(L2)로 발화/해소한다. 채널 정책 게이트
 *        403 계수(AddChannelPolicyViolation)와 sec-agree 협상 거절(494/421) 계수
 *        (AddSecAgreeReject)도 같은 윈도우에서 급증 임계로 평가한다.
 *        A-QOS-006/007/009/011·A-SEC-003/004 — 임계는 모듈 설정 소유
 *        (docs/design/alarm_self_reporting.md §4).
 *
 *        율 산식 (윈도우 내 INVITE/REGISTER 최종응답 — 수신+송신 합산. 수신에는 와이어에
 *        나가지 않는 트랜잭션 로컬 합성 응답(408 Timer B/Ring timeout, 660 connect error)이
 *        포함된다 — flow 로그 사각의 보완이 이 축의 존재 이유):
 *          - 호: 분모 = 최종응답 - 인증 챌린지(401/407). 유효 = 2xx/3xx +
 *            사용자 행위 결과(486 busy / 487 cancel / 603 decline) — 시스템 장애가 아닌
 *            결과를 실패로 세지 않는다 (NER 계열, ITU-T E.425 준용).
 *          - 등록: 분모 = 최종응답 - 챌린지/협상(401/407/423). 유효 = 2xx.
 *        분모 < MinFinals 이면 판정 표본 부족 — 발화하지 않고 열린 알람은 해소한다
 *        (저트래픽 구간의 소표본 잡음 억제).
 */
class CSipStatsMonitor {
public:
    CSipStatsMonitor();

    /** 메인 루프 1s tick — EvalSec 경과 시 평가. SIGUSR1 리로드 값은 다음 평가부터 반영 */
    void Poll( time_t tNow );

    /** monitor 명령(sip_stats) — 누적 카운터 + 최근 윈도우 평가 결과 */
    void GetString( CMonitorString &strBuf );

    /** 채널 정책 게이트 403 계수 (A-SEC-003 — CscfModule::CheckChannelPolicy 가 SIP worker
     *  스레드에서 호출. 소스 로그 억제와 무관하게 전 건 계수, 소스 IP 상위 추적) */
    void AddChannelPolicyViolation( const char *pszIp );

    /** sec-agree 협상 거절(494/421) 계수 (A-SEC-004 — CscfModule::SendSecAgreeReject 가 SIP worker
     *  스레드에서 호출. 변조·제안 없음·Verify 생략·정책 위반 전 건, 소스 IP 상위 추적) */
    void AddSecAgreeReject( const char *pszIp );

private:
    struct RateResult {
        uint64_t ulFinals;     // 분모 (제외 코드 차감 후)
        uint64_t ulEffective;  // 유효 결과 수
        int iTopFailCode;      // 최다 실패 응답 코드 (0 = 실패 없음)
        uint64_t ulTopFailCount;
        double dRate;  // 유효율 % (분모 0 이면 100)

        RateResult() : ulFinals( 0 ), ulEffective( 0 ), iTopFailCode( 0 ), ulTopFailCount( 0 ), dRate( 100.0 ) {
        }
    };

    void Evaluate( time_t tNow, int iWindowSec );
    void CloseAll();
    static RateResult CalcRate( const CSipStackCounter::Snapshot &clsDelta, CSipStackCounter::EGroup eGroup,
                                const int *piExclude, int iExcludeCount, const int *piEffective, int iEffectiveCount,
                                int iEffectiveBelow );
    void FireRateAlarm( const char *pszCode, const char *pszComponent, const RateResult &clsRate, int iMinFinals,
                        int iMinor, int iMajor, int iCritical, int iWindowSec );

    static const int VIOLATION_SRC_MAX = 32;  // 보안 위반 소스 IP 추적 상한 (평가 시 리셋)

    /** 보안 위반 소스 계수기 — SIP worker 스레드(Add) ↔ 메인 루프(Drain, 윈도우 리셋).
     *  A-SEC-003(채널 정책 403)·A-SEC-004(sec-agree 494/421) 가 같은 모양을 쓴다. */
    struct SourceCounter {
        std::mutex clsMutex;
        uint64_t ulTotal = 0;                    // 누적 (monitor 노출용)
        uint64_t ulWindow = 0;                   // 현재 윈도우 계수 (평가 시 리셋)
        std::map<std::string, uint64_t> mapSrc;  // 윈도우 소스별 계수

        void Add( const char *pszIp );
        void Reset();
        /** 윈도우 계수를 꺼내고 리셋 — 최다 소스와 그 건수 동반 */
        uint64_t Drain( std::string &strTopSrc, uint64_t &ulTopCount );
        uint64_t Total();
    };

    std::mutex m_clsMutex;  // 최근 윈도우 결과 보호 — Poll(메인 루프) ↔ GetString(monitor 스레드)
    CSipStackCounter::Snapshot m_clsPrev;
    time_t m_tLastEval;  // 0 = 기준 스냅샷 미확보 (평가 off 포함)

    SourceCounter m_clsChannelPolicy;   // 채널 정책 게이트 403 (A-SEC-003)
    SourceCounter m_clsSecAgreeReject;  // sec-agree 협상 거절 494/421 (A-SEC-004)

    // 최근 윈도우 결과 (monitor 노출용)
    int m_iLastWindowSec;
    RateResult m_clsLastCall;
    RateResult m_clsLastReg;
    double m_dLastCps;
    uint64_t m_ulLastParseError;
    std::string m_strLastTopSrc;
    uint64_t m_ulLastChannelPolicy;
    std::string m_strLastViolationTopSrc;
    uint64_t m_ulLastSecAgreeReject;
    std::string m_strLastSecAgreeTopSrc;
};

extern CSipStatsMonitor gclsSipStatsMonitor;

#endif
