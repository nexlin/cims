#ifndef _CSP_PHONE_GROUP_H_
#define _CSP_PHONE_GROUP_H_

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

/**
 * @ingroup CspServer
 * @brief 전화 그룹 멤버 (phone_group_members) — alert_order 는 sequential 호출·MaxForkTargets 절삭 순서.
 */
struct CspPhoneGroupMember {
    std::string strUserId;
    int iAlertOrder = 0;
};

/**
 * @ingroup CspServer
 * @brief 전화 그룹 (dispatch_center.md §3.1) = 픽업 그룹 + (선택) 대표번호. 유선 전화의 일반 기능 — 관제 권한과 무관.
 *
 * id 는 불변 키(CSC 발급 pg-xxxxxxxx, 전환 전 dg-… 유지)이며 *_subscriptions.pickup_group 값과 같다 — 당겨받기·
 * 그룹원 BLF(CanWatch 규칙 1)·대표번호 병렬 호출이 이 한 축을 공유한다. 감청·청취·관리 범위는 여기 없다(역할 —
 * CspRole.h).
 */
class CspPhoneGroup {
public:
    std::string m_strId;
    std::string m_strName;
    std::string m_strPilotId;         ///< 대표번호(AoR user part). 빈 값=대표번호 없음
    std::string m_strServiceRef;      ///< 대표번호 접속서비스 name (유선 VoIP — 도메인·SRTP 정책 근거)
    std::string m_strAlertMode;       ///< parallel(기본) | sequential (TS 24.239)
    int m_iNoAnswerSec;               ///< 전원 무응답 판정 초 (Setup.Sip.Dispatch.ForkRingTimeoutSec 로 clamp)
    std::string m_strBusyMembers;     ///< skip(기본) | alert
    std::string m_strOverflowTarget;  ///< 무응답 넘김 대상(대표번호/가입 번호). 빈 값=480
    std::string m_strOrgId;

    /** 멤버 — alert_order 오름차순. 포크 대상 결정의 SoT (등록 여부는 UserMap 으로 판정). */
    std::vector<CspPhoneGroupMember> m_vecMembers;

    CspPhoneGroup() {
        Clear();
    }
    void Clear();

    bool HasPilot() const {
        return !m_strPilotId.empty();
    }
    bool IsMember( const std::string &strUserId ) const;

    /** JSON fallback 파일(DataFolder.PhoneGroup/<id>.json) 로드 — id 는 파일명. */
    bool LoadFile( const std::string &strPath );
};

/**
 * @ingroup CspServer
 * @brief 전화 그룹 인메모리 맵 (dispatch_center.md §3.5) — 그룹 id 인덱스 + pilot 인덱스 + 멤버 인덱스.
 *
 * 부팅 시 DbManager 가 적재하고 PHONE_GROUP_CHANGED 통지로 재적재한다. DB 불가 시 JSON fallback
 * (DataFolder.PhoneGroup). INVITE 경로의 판정(pilot 해석·픽업 축)은 전부 이 맵에서 답한다 — DB 질의 금지.
 */
class CCspPhoneGroupMap {
public:
    bool LoadFromDb();
    bool LoadOneFromDb( const char *pszGroupId );
    bool Load( const char *pszDirName );

    void Insert( const CspPhoneGroup &clsGroup );
    void Remove( const char *pszGroupId );
    void Clear();

    bool Select( const char *pszGroupId, CspPhoneGroup &clsGroup );
    bool Contains( const char *pszGroupId );
    /** 대표번호로 그룹 조회 — pilot 해석(§4.2). */
    bool SelectByPilot( const char *pszPilotId, CspPhoneGroup &clsGroup );
    /** 멤버십으로 그룹 조회 — 가입자당 그룹 하나(§3.2). */
    bool SelectForUser( const char *pszUserId, CspPhoneGroup &clsGroup );
    /** 대표번호인가(그룹 존재). */
    bool IsPilot( const char *pszId );

    /** 가입자의 전화 그룹 id — 멤버 인덱스 우선, 없으면 빈 값. */
    std::string GroupIdForUser( const char *pszUserId );
    /** 가입자의 유효 그룹 축 값 — 멤버 인덱스, 없으면 CspUser.EffectivePickupGroup()(pickup_group 만, org 폴백 없음).
     *  픽업·BLF 규칙 1·감청 대상 그룹 판정이 모두 이 한 값을 쓴다 (dispatch_center.md §3.2·§3.5). */
    std::string EffectiveGroupOf( const char *pszUserId );

    int GetCount();

private:
    std::map<std::string, CspPhoneGroup> m_clsMap;
    std::map<std::string, std::string> m_clsPilotIndex;   ///< pilot → group id
    std::map<std::string, std::string> m_clsMemberIndex;  ///< user → group id
    std::recursive_mutex m_clsMutex;

    void _index( const CspPhoneGroup &clsGroup );
    void _unindex( const CspPhoneGroup &clsGroup );
};

extern CCspPhoneGroupMap gclsPhoneGroupMap;

#endif
