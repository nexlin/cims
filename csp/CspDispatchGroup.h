#ifndef _CSP_DISPATCH_GROUP_H_
#define _CSP_DISPATCH_GROUP_H_

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

/**
 * @ingroup CspServer
 * @brief 관제 그룹 멤버 (dispatch_group_members) — alert_order 는 sequential 호출·MaxForkTargets 절삭 순서.
 */
struct CspDispatchMember {
    std::string strUserId;
    int iAlertOrder = 0;
};

/**
 * @ingroup CspServer
 * @brief 관제 그룹 (dispatch_center.md §3) = 픽업 그룹 + (선택) 대표번호 + (선택) 감청 범위.
 *
 * id 는 불변 키(CSC 발급 dg-xxxxxxxx)이며 volte_subscriptions.pickup_group 값과 같다 — 당겨받기·BLF 인가·
 * 대표번호 병렬 호출·감청 범위가 이 한 축을 공유한다. 대표번호가 없으면 순수 당겨받기 그룹이다.
 */
class CspDispatchGroup {
public:
    std::string m_strId;
    std::string m_strName;
    std::string m_strPilotId;           ///< 대표번호(AoR user part). 빈 값=대표번호 없음
    std::string m_strServiceRef;        ///< 대표번호 접속서비스 name (도메인·SRTP 정책 근거)
    std::string m_strAlertMode;         ///< parallel(기본) | sequential (TS 24.239)
    int m_iNoAnswerSec;                 ///< 전원 무응답 판정 초 (Setup.Sip.Dispatch.ForkRingTimeoutSec 로 clamp)
    std::string m_strBusyMembers;       ///< skip(기본) | alert
    std::string m_strOverflowTarget;    ///< 무응답 넘김 대상(대표번호/내선). 빈 값=480
    std::string m_strMonitorScope;      ///< none(기본) | own | listed | all
    std::string m_strPttListen;         ///< none(기본) | listed | all
    std::string m_strListenVisibility;  ///< hidden(기본) | visible
    std::string m_strOrgId;

    /** 멤버 — alert_order 오름차순. 포크 대상 결정의 SoT (등록 여부는 UserMap 으로 판정). */
    std::vector<CspDispatchMember> m_vecMembers;
    /** monitor_scope=listed 의 감청 대상 그룹 id */
    std::set<std::string> m_setMonitorTargets;
    /** ptt_listen=listed 의 청취 대상 PTT 그룹 (mcptt_group_id) */
    std::set<std::string> m_setPttTargets;

    CspDispatchGroup() {
        Clear();
    }
    void Clear();

    bool HasPilot() const {
        return !m_strPilotId.empty();
    }
    bool IsMember( const std::string &strUserId ) const;

    /** JSON fallback 파일(csp/DispatchGroup/<id>.json) 로드 — id 는 파일명. */
    bool LoadFile( const std::string &strPath );
};

/**
 * @ingroup CspServer
 * @brief 관제 그룹 인메모리 맵 (dispatch_center.md §3.3) — 그룹 id 인덱스 + pilot 인덱스 + 멤버 인덱스.
 *
 * 부팅 시 DbManager 가 적재하고 DISPATCH_GROUP_CHANGED 통지로 재적재한다. DB 불가 시 JSON fallback
 * (DataFolder.DispatchGroup). INVITE 경로의 판정(pilot 해석·감청 범위)은 전부 이 맵에서 답한다 — DB 질의 금지.
 */
class CCspDispatchGroupMap {
public:
    bool LoadFromDb();
    bool LoadOneFromDb( const char *pszGroupId );
    bool Load( const char *pszDirName );

    void Insert( const CspDispatchGroup &clsGroup );
    void Remove( const char *pszGroupId );
    void Clear();

    bool Select( const char *pszGroupId, CspDispatchGroup &clsGroup );
    bool Contains( const char *pszGroupId );
    /** 대표번호로 그룹 조회 — pilot 해석(§4.2). */
    bool SelectByPilot( const char *pszPilotId, CspDispatchGroup &clsGroup );
    /** 멤버십으로 그룹 조회 — 가입자당 그룹 하나(§3.2). */
    bool SelectForUser( const char *pszUserId, CspDispatchGroup &clsGroup );
    /** 대표번호인가(그룹 존재). */
    bool IsPilot( const char *pszId );

    /** 가입자의 관제 그룹 id — 멤버 인덱스 우선, 없으면 빈 값. */
    std::string GroupIdForUser( const char *pszUserId );
    /** 가입자의 유효 그룹 축 값 — 멤버 인덱스, 없으면 CspUser.EffectivePickupGroup()(pickup_group→org 폴백).
     *  픽업·BLF·감청 인가가 모두 이 한 값을 쓴다 (dispatch_center.md §3.2). */
    std::string EffectiveGroupOf( const char *pszUserId );

    /** dialog 이벤트·Join 인가 범위 (§5.2 CanWatchDialog). strWatcherGroup/strTargetGroup 은 관제 그룹 id
     *  (픽업 그룹 값). 같은 그룹이면 허용(현행), 아니면 watcher 그룹의 monitor_scope 로 판정. */
    bool CanWatch( const std::string &strWatcherGroup, const std::string &strTargetGroup );

    /** PTT 그룹콜 청취 범위 (§5.6 ptt_listen) — watcher 그룹이 pszPttGroupId 를 들을 수 있는가. */
    bool CanListenPtt( const std::string &strWatcherGroup, const char *pszPttGroupId );

    int GetCount();

private:
    std::map<std::string, CspDispatchGroup> m_clsMap;
    std::map<std::string, std::string> m_clsPilotIndex;   ///< pilot → group id
    std::map<std::string, std::string> m_clsMemberIndex;  ///< user → group id
    std::recursive_mutex m_clsMutex;

    void _index( const CspDispatchGroup &clsGroup );
    void _unindex( const CspDispatchGroup &clsGroup );
};

extern CCspDispatchGroupMap gclsDispatchGroupMap;

#endif
