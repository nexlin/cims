/*
 * CscEndpointCache — CSC 로부터 취득하는 "단말용 MCPTT 서비스 주소" 캐시
 *
 *   GET {Setup.Csc.Scheme}://{Host}:{Port}/internal/mcptt/endpoint
 *       Authorization: Bearer {Setup.Csc.InternalToken}
 *   → {"xcap_root":"https://host:4430/","mcptt_port":4430,"public_url_configured":bool}
 *
 * 단말이 XCAP 문서(그룹/user-profile/service-config)를 받는 주소의 정본은 **CSC**
 * (`McpttServer.PublicUrl`) 한 곳이다. CSP 에는 그 주소를 적는 설정이 없다 — 과거
 * `Setup.Xcap.{Host,Port,Scheme}` 는 폐기됐다(CSC 속성이 CSP 설정에 복제되어 있어
 * ue-init-config 가 알려주는 주소와 어긋날 수 있었다).
 *
 * 소비처: xcap-diff NOTIFY 의 `xcap-root` · MCData FD 다운로드 URL base.
 * 갱신 시점: 기동 시 1회 · SIGUSR1 reload · CSC_RESTART 수신. 실패 시 마지막 성공값을
 * 유지하고, 그것도 없으면 `Setup.Csc` 주소 + 기본 포트 4430 으로 유도한다(ERROR 로그).
 */
#ifndef _CSC_ENDPOINT_CACHE_H_
#define _CSC_ENDPOINT_CACHE_H_

#include <time.h>

#include <mutex>
#include <string>

namespace CscEndpoint {

    /** CSC admin 서버 base URL — {Setup.Csc.Scheme}://{Host|LocalIp}:{Port} (후행 '/' 없음). */
    std::string AdminBaseUrl();

}  // namespace CscEndpoint

class CCscEndpointCache {
public:
    /** 단말용 XCAP root (후행 '/' 포함). 미취득이면 1회 조회 시도 후 last-known/유도값. */
    std::string GetXcapRoot();

    /** 단말용 서비스 URL base (후행 '/' 없음) — MCData FD 등 경로를 붙일 때. */
    std::string GetServiceUrlBase();

    /** CSC 재조회. 성공 시 true. 기동/SIGUSR1/CSC_RESTART 경로에서 호출. */
    bool Refresh();

private:
    /** 설정 유도값 — CSC 주소 + 기본 MCPTT 포트. 조회 실패 시의 최후 폴백. */
    std::string Derive();
    bool Fetch( std::string &strOut );

    std::mutex m_clsMutex;
    std::string m_strXcapRoot;  // 마지막 성공값 (빈 문자열 = 미취득)
    time_t m_tLastAttempt = 0;  // 실패 후 재시도 억제 (스탬피드 방지)
};

extern CCscEndpointCache gclsCscEndpointCache;

#endif
