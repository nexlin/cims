#ifndef _SIP_STATS_MONITOR_H_
#define _SIP_STATS_MONITOR_H_

#include <time.h>

#include <mutex>
#include <string>

#include "SipStackCounter.h"

class CMonitorString;

/**
 * @ingroup CspServer
 * @brief SIP 신호 통계 감시 — 스택 카운터(CSipStack::m_clsCounter)를 평가 윈도우
 *        (Setup.SipStats.EvalSec) 단위로 차분해 호/등록 성공률·신규 INVITE CPS·SIP 수신
 *        이상을 단계 임계로 평가하고 FM 자기보고(L2)로 발화/해소한다.
 *        A-QOS-006/007/009/011 — 임계는 모듈 설정 소유 (docs/design/alarm_self_reporting.md §4).
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

    std::mutex m_clsMutex;  // 최근 윈도우 결과 보호 — Poll(메인 루프) ↔ GetString(monitor 스레드)
    CSipStackCounter::Snapshot m_clsPrev;
    time_t m_tLastEval;  // 0 = 기준 스냅샷 미확보 (평가 off 포함)

    // 최근 윈도우 결과 (monitor 노출용)
    int m_iLastWindowSec;
    RateResult m_clsLastCall;
    RateResult m_clsLastReg;
    double m_dLastCps;
    uint64_t m_ulLastParseError;
    std::string m_strLastTopSrc;
};

extern CSipStatsMonitor gclsSipStatsMonitor;

#endif
