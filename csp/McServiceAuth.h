#ifndef _MC_SERVICE_AUTH_H_
#define _MC_SERVICE_AUTH_H_

#include <time.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "McService.h"

// ── MC 서비스 인가·서비스 설정 (TS 24.379 §7.3 · TS 24.282 §7.3 · TS 24.281 §7.3) ──
//  서비스 인가 = 단말이 실은 접근 토큰(TS 33.180 Annex B — IdMS 발급)을 IdMS 에 introspection(RFC 7662)으로 검증해
//  (MC ID, MC client ID, IMPU) 바인딩을 만든다(§7.3.2 4) · §7.3.3 5)). 토큰은 REGISTER 본문(§7.3.2) 또는 poc-settings
//  PUBLISH(§7.3.3)로 온다. 서비스 설정 = poc-settings(RFC 4354 · §7.4.1.2.2) — Answer-Mode Indication · 선택 user
//  profile · multiplex 지원을 클라이언트마다 캐시한다(§7.3.3 8)·10)·11)). CIMS 는 단일 MC service ID(TS 23.280
//  §10.1.4.1 — mcx_identity_scope.md §1)라 MC ID = IMPU 사용자부다. 토큰의 MC ID 가 요청 IMPU 와 다르면 인가 실패(101).
//  바인딩은 그 IMPU 의 등록과 함께 산다 — 등록 해제·서비스 태그를 뺀 재등록(§7.1 · §7.2.1 NOTE 1)·설정 제거
//  PUBLISH(§7.3.5)가 지운다.

/** poc-settings 의 <entity> 하나 (RFC 4354 · TS 24.379 §7.4.1.2.2). */
struct McPocEntity {
    std::string strId;           ///< entity id = MC client 의 Instance ID URN (§7.2.1A NOTE 2)
    std::string strAnswerMode;   ///< <am-settings><answer-mode> = "automatic" | "manual", 없으면 ""
    int iUserProfileIndex = -1;  ///< mcs10Set:selected-user-profile-index/user-profile-index, 없으면 -1
    bool bHasMultiplex = false;  ///< mcs10Set:multiplex-support 요소가 있었다
    bool bMultiplex = false;
};

/** application/poc-settings+xml 의 <entity> 목록 — 요소 이름은 접두사와 무관하게 읽는다. */
std::vector<McPocEntity> ParsePocSettings( const std::string &strXml );

/** NOTIFY 본문(§7.3.6.2) — 한 사용자의 클라이언트들(entity 마다 am-settings · 선택 user profile · multiplex). */
std::string BuildPocSettingsDoc( const std::vector<McPocEntity> &vecEntities );

/** 서비스 사용자 자격 scope (TS 33.180 B.4.2.2 — mcx_identity_scope.md §3) */
const char *McServiceScope( EMcService e );

/** MC 서비스 info 본문의 subtype·요소 접두 (mcptt-info · mcdata-info · mcvideo-info) */
const char *McInfoSubtype( EMcService e );
const char *McInfoPrefix( EMcService e );

/** 서비스 인가 200 OK 본문 — 그 MC ID 의 바인딩이 둘 이상이면 info 문서 <multiple-devices-ind>true (TS 24.379 §7.3.3
 * 9)a) · TS 24.282 §7.3.3 · TS 24.281 §7.3.3, Annex F.1·D.1 contentType = <…Boolean>). */
std::string McMultipleDevicesDoc( EMcService e );

enum class EMcAuthResult { Ok, Failed, Unavailable };

/** IdMS introspection 응답(RFC 7662 JSON)으로 서비스 인가를 판정한다 — active · 서비스 scope · MC ID(MCData =
 * mcdata_id, 그 밖 = mcptt_id — 단일 MC service ID) = strImpuUser. strWhy = 실패 사유(로그). 응답이 JSON 이 아니면
 * Unavailable. */
EMcAuthResult McAuthVerdict( const std::string &strIntrospectJson, EMcService e, const std::string &strImpuUser,
                             std::string &strWhy );

/** (MC ID, MC client ID) 바인딩 하나와 그 클라이언트의 서비스 설정 */
struct McServiceBinding {
    std::string strClientId;
    bool bSettings = false;  ///< poc-settings 를 받았다(Answer-Mode Indication — §10.1.1.3.2 3) 146 판정)
    McPocEntity clsSettings;
    time_t tSettings = 0;
};

class CMcServiceAuth {
public:
    /** 접근 토큰으로 서비스 인가 — IdMS introspection(McAuthVerdict) 성공이면 (e, strImpuUser, strClientId) 를
     * 바인딩한다 (같은 client ID 의 바인딩은 갱신, 다른 client ID 는 더한다 — §7.3.3 5)b)). */
    EMcAuthResult Authorize( EMcService e, const std::string &strImpuUser, const std::string &strToken,
                             const std::string &strClientId, std::string &strWhy );
    /** 바인딩이 있는가 — strClientId 가 비면 그 MC ID 의 아무 클라이언트 */
    bool HasBinding( EMcService e, const std::string &strMcId, const std::string &strClientId = "" ) const;
    int BindingCount( EMcService e, const std::string &strMcId ) const;
    /** 바인딩된 클라이언트의 서비스 설정 캐시(§7.3.3 8) · §7.3.4 8)) — 바인딩이 없으면 false */
    bool SetSettings( EMcService e, const std::string &strMcId, const std::string &strClientId,
                      const McPocEntity &clsEntity );
    /** 그 MC ID 클라이언트들 가운데 가장 최근에 받은 Answer-Mode("automatic"|"manual") — 없으면 false */
    bool AnswerModeOf( EMcService e, const std::string &strMcId, std::string &strMode ) const;
    /** 바인딩 제거 — strClientId 가 비면 그 MC ID 의 모든 클라이언트. 지운 수 */
    int Unbind( EMcService e, const std::string &strMcId, const std::string &strClientId = "" );
    /** 등록이 사라졌다 — 모든 서비스의 바인딩 제거 */
    void UnbindAll( const std::string &strMcId );
    /** NOTIFY 본문(§7.3.6.2) — 그 MC ID 의 설정을 받은 클라이언트 전부 */
    std::string SettingsDocument( EMcService e, const std::string &strMcId ) const;
    /** IdMS introspection 주소 — CSC 공개 base + /idms/introspect (mcx_identity_scope.md §7) */
    static std::string IntrospectUrl();

private:
    mutable std::mutex m_mutex;
    std::map<std::pair<int, std::string>, std::vector<McServiceBinding>> m_map;  ///< (서비스, MC ID) → 클라이언트들
};

extern CMcServiceAuth gclsMcServiceAuth;

#endif  // _MC_SERVICE_AUTH_H_
