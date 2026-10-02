/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com> (http://blog.naver.com/websearch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _CSP_USER_H_
#define _CSP_USER_H_

#include <strings.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "CspServerDefine.h"
#include "SipMutex.h"

/**
 * @ingroup CspServer
 * @brief 사용자 MCPTT 프로파일 (ptt_user_profile — TS 24.484 / TS 24.379 §6.3.3.1.13.2).
 *        행 부재 시 기본값 = 모드 DedicatedGroup + 긴급그룹 미지정(긴급 미인가) + 인가 전부 허용.
 */
struct CspUserProfile {
    bool m_bAllowEmergencyCall = true;   ///< allow-emergency-group-call (긴급 그룹콜 개시 인가)
    bool m_bAllowEmergencyAlert = true;  ///< allow-activate-emergency-alert (경보 개시 인가)
    /** allow-cancel-group-emergency (TS 24.484 ruleset) — 그룹의 진행 중 긴급 상태 해제 인가. 서버 판정은 local policy
     *  (TS 24.379 §6.3.3.1.13.4) = 개시자 ∨ 이 값. 기본 false — 개시자만 해제한다(관제사에게 켠다). */
    bool m_bAllowCancelGroupEmergency = false;
    /** allow-cancel-imminent-peril (TS 24.484 ruleset) — 임박 위험 해제 인가(§6.3.3.1.13.6 — 이 값만으로 판정). 기본
     * true. */
    bool m_bAllowCancelImminentPeril = true;
    /** allow-cancel-emergency-alert (TS 24.484 ruleset) — 긴급 경보 취소 인가(§6.3.3.1.13.3). 컬럼 미적용 DB 에서는
     *  발령 인가(allow_emergency_alert) 값 — 그 전 문서가 같은 값을 냈다. */
    bool m_bAllowCancelEmergencyAlert = true;
    bool m_bAllowAdhocCall = true;                           ///< ad hoc 개시 인가 (Setup.PttAdhocEnabled 와 AND)
    std::string m_strEmergencyGroupMode = "DedicatedGroup";  ///< entry-info: DedicatedGroup|UseCurrentlySelectedGroup
    std::string m_strEmergencyGroupId;                       ///< 전용 긴급그룹 (mcptt_group_id, 빈 값=미지정)
    bool m_bAllowEmergencyPrivateCall = true;                ///< allow-emergency-private-call (긴급 사설콜 개시 인가)
    std::string m_strPrivateEmergencyMode =
        "LocallyDetermined";  ///< MCPTTPrivateRecipient entry-info: LocallyDetermined|UsePreConfigured
    std::string m_strEmergencyPrivateRecipient;  ///< 사전 지정 긴급 수신자 (UsePreConfigured 모드, 빈 값=미지정)
    /** allow-ambient-listening (TS 24.484 ruleset) — 원격 청취 수행 자격 (관제사, dispatch_center.md §5.6).
     *  기본 false — 행 부재·컬럼 미적용 DB 에서는 전원 자격 없음(청취 INVITE 403). */
    bool m_bAllowAmbientListening = false;
    /** allow-to-receive-non-acknowledged-users-information (TS 24.484 anyExt) — 그룹 호 개시자로서 확인 통화 설정이
     * 필수 멤버 없이 진행됐을 때 응답하지 않은 멤버 목록 INFO 를 받을 자격 (TS 24.379 §6.3.3.3). 기본 false. */
    bool m_bAllowNonAckUsersInfo = false;
    /** 개별 호 (TS 24.484 ruleset — CSC user profile 과 같은 열). 발신 = allow-private-call(수동·자동 개시 인가도 같은
     * 값 — CSC 가 그렇게 낸다), 목록 밖 상대 = allow-private-call-to-any-user, 착신 참가 =
     * allow-private-call-participation. 행 부재·컬럼 미적용 = 허용. 판정 = TS 24.379 §11.1.1.3.1.1 10)·11) ·
     * §11.1.1.3.2 8) (403 107·144·127). */
    bool m_bAllowPrivateCall = true;
    bool m_bAllowPrivateCallToAnyUser = true;
    bool m_bAllowPrivateCallParticipation = true;
};

/** MCVideo 이용 자격 — mcvideo_user_profile 행(TS 24.484 §9.3, docs/design/features/mcvideo.md §5.1). 행이 없으면
 * 자격이 없다 — MCVideo 서비스 인가 실패(TS 24.281 §7.3.2, Warning 101). MCVideo ID = MCPTT ID(§7 D1). */
struct CspMcVideoProfile {
    int m_iMaxVideoStreams = 1;  ///< <MaxSimultaneousVideoStreams> = 서버 카운터 C9 (CMP PTT_JOIN max_rx_streams)
    int m_iMaxCallsN6 = 1;       ///< MCVideo 그룹 호 동시 상한 N6 (TS 24.281 §9.2.2.3.1.1 5) — 486 Warning 103)
    /// <MaxAffiliationsN2> — 동시 MCVideo 제휴 그룹 상한 N2(TS 24.281 §8.2.2.2.3 14)c) — 넘는 제휴 요청은 줄인다 ·
    /// §9.2.2.3.1.1 7) chat 개시의 암묵적 제휴는 486 Warning 102). 열이 없는 DB(migrate_mcvideo_n2.sql 전)는 기본 4
    int m_iMaxAffiliationsN2 = 4;
};

/**
 * @ingroup CspServer
 * @brief SIP 사용자 정보 저장 클래스
 */
class CspUser {
public:
    CspUser() : m_bIcbAll( false ) {
        m_iCreateTime = 0;
        m_iUpdateTime = 0;
        m_iRegisterTime = 0;
        m_iLogoutTime = 0;
    };
    ~CspUser() {};

    std::string m_strId;

    // 표시 이름
    std::string m_strName;

    // SIP 인증용 아이디 (IMS 등에서 전화번호와 분리된 단말기 고유 ID. 없으면 m_strId와 동일시함)
    std::string m_strAuthId;

    // SIP 비밀번호 (평문). DB 가입자 경로에서는 읽지 않는다(passwd 컬럼 DROP — sip_access_security.md §4.7 ⑥).
    //   남은 소비자: 원격 노드(peer) outbound 인증 자격(ModuleDispatcher 의 RouteConfig.auth_password)과
    //   JSON 파일 fallback(csp/User/*.json 의 "passwd").
    std::string m_strPassWord;

    // SIP Digest H(A1) = MD5(impi:realm:password) — 인증 자료 SoT (sip_access_security.md §4).
    //   DB 가입자는 이 값만으로 인증한다. 비어 있으면 m_strPassWord (JSON 파일 fallback 에서만 채워진다).
    std::string m_strHa1;

    // 채널 정책 (sip_access_security.md §3.1) — DB sip_transport ENUM('UDP','TCP','TLS') / NULL.
    //   "TLS" 만 서버가 집행한다(비-TLS 채널의 이 신원 요청은 403). 나머지는 프로비저닝 힌트.
    std::string m_strSipTransport;

    // 인증 체계 (sip_access_security.md §8.2) — DB auth_scheme 'digest'(기본) | 'aka'. Cx 의
    //   SIP-Authentication-Scheme 상당: 챌린지 체계는 협상이 아니라 프로비저닝으로 확정된다(TS 33.203 Annex P.4).
    std::string m_strAuthScheme;

    /** IMS AKA 가입자인가. */
    bool isAka() const {
        return strcasecmp( m_strAuthScheme.c_str(), "aka" ) == 0;
    }

    /** TLS 채널 강제 대상 가입자인가 — sip_transport=TLS 정책, 또는 IMS AKA(Annex X — TLS 위에서만 성립). */
    bool requiresTls() const {
        return strcasecmp( m_strSipTransport.c_str(), "TLS" ) == 0 || isAka();
    }

    // v3 (2026-04-22): 서비스 귀속을 name 기반 참조로 이전.
    //   - m_strServiceRef = access_services.name (빈 문자열이면 REGISTER 거부)
    //   - service.domain 과 결합하여 Digest username (full IMPI) 구성
    //   - m_strImsi 가 비면 m_strAuthId 를 fallback 으로 사용
    std::string m_strServiceRef;  // access_services.name 참조
    std::string m_strImsi;

    // 착신 차단 (ICB — TS 24.611, volte_supplementary_services.md §6B) — 규칙 둘.
    //   전체 = 회선 icb_all(모든 착신) / 지정 번호 = 사람 icb_identities(cp:identity — 그 사람의 모든 전화 회선)
    bool m_bIcbAll;
    std::vector<std::string> m_vecIcbIdentities;

    // 착신전환 (TS 24.604 CDIV — volte_supplementary_services.md §6A) — CFU / CFB / CFNR(+시한) / CFNL. 값 =
    // 번호(다이얼 플랜 번역 전)
    std::string m_strForward;              // CFU  forward_id
    std::string m_strForwardBusy;          // CFB  forward_busy_id (486/600·Q.850 17)
    std::string m_strForwardNoReply;       // CFNR forward_no_reply_id (링잉 뒤 무응답 시한·480/408)
    int m_iForwardNoReplySec = 0;          // CFNR 시한(초, 0 = Setup.Sip.Cdiv.NoReplySec)
    std::string m_strForwardNotLoggedIn;   // CFNL forward_not_logged_in_id (INVITE 때 미등록)
    std::string m_strForwardNotReachable;  // CFNRc forward_not_reachable_id (Q.850 20 · 링잉 없이 480/408)

    // 서비스 타입: "volte" | "ptt" | "both"
    std::string m_strServiceType;

    // 소속 아이디
    std::string m_strOrganizationId;

    // 당겨받기 그룹 키 (volte_supplementary_services.md §5.1) — 같은 값끼리 픽업 가능.
    //   빈 값 = 어떤 픽업·BLF 축에도 속하지 않는다(org 폴백 없음 — 같은 조직이라는 사실만으로 남의 호를
    //   당겨받거나 dialog 를 구독할 수 없다. volte_supplementary_services.md §5.1).
    std::string m_strPickupGroup;
    /** 가입자 링백 음원 id(sys:|op:|sub: — announcements.md §6.3). 비면 접속서비스 프로파일의 ringback 그대로 */
    std::string m_strRingbackMedia;

    /** 등록 바인딩·픽업 판정에 쓰는 유효 픽업 그룹 — pickup_group 우선, 비면 org 폴백. */
    const std::string &EffectivePickupGroup() const {
        return m_strPickupGroup;
    }

    // 가입자가 생성된 시간
    time_t m_iCreateTime;
    // 가입자 정보가 마지막으로 수정된 시간
    time_t m_iUpdateTime;

    // 마지막 Register 시간
    time_t m_iRegisterTime;
    // 마지막 Logout 시간
    time_t m_iLogoutTime;

    /** 착신 차단 판정 — 걸린 규칙 이름("all"|"identity"), 차단 아니면 nullptr (TS 24.611 §4.5.2.6.1 — 603).
     *  vecCallerIds = 발신자 신원 후보(P-Asserted-Identity, From — 규격의 cp:identity 대조 대상), fnNorm = 비교 전
     *  번호 정규화(+E.164). 후보 하나라도 목록과 같으면 차단이다 — 후보를 늘리는 쪽은 차단을 넓힐 뿐 우회가 되지
     * 않는다. */
    template <class FnNorm>
    const char *IncomingBarredBy( const std::vector<std::string> &vecCallerIds, FnNorm fnNorm ) const {
        if ( m_bIcbAll ) return "all";
        if ( m_vecIcbIdentities.empty() ) return nullptr;
        for ( const auto &strCaller : vecCallerIds ) {
            if ( strCaller.empty() ) continue;
            const std::string strNorm = fnNorm( strCaller );
            for ( const auto &strBarred : m_vecIcbIdentities )
                if ( strBarred == strCaller || fnNorm( strBarred ) == strNorm ) return "identity";
        }
        return nullptr;
    }
    bool isCallForward() {
        return m_strForward.empty() == false;
    };
    bool hasConditionalForward() const {
        return !m_strForwardBusy.empty() || !m_strForwardNoReply.empty() || !m_strForwardNotLoggedIn.empty() ||
               !m_strForwardNotReachable.empty();
    }

    // bool Parse( const char *pszFileName );
    void clear();

    friend class CspUserMap;
    friend class CDbManager;

private:
    time_t _loadTime;
    // bool IsDnd();
    // bool IsCallForward();
};

// 가입자 정보를 관리하는 클래스
// Caching User Data
typedef std::map<std::string, CspUser> CSP_USER_MAP;

class CspUserMap {
public:
    // isUser : alive user
    bool isAlive( std::string strToId, CspUser &clsUser );
    bool select( std::string strToId, CspUser &clsUser );

    bool registerUser( std::string strUserId, std::string strPassWord );
    /** 등록 해제를 반영한다(logout_time·Redis 바인딩). bReclaimAffiliations = false 면 제휴는 남긴다
     *  (flow 실패 유예 — AffiliationGrace.h). */
    bool unregisterUser( std::string strUserId, bool bReclaimAffiliations = true );
    bool Select( const char *pszUserId, CspUser &clsXmlUser );
    void Insert( CspUser &clsXmlUser );
    bool Load( const char *pszDirName );
    /** DB 전량 적재. @param pbUnavailable (선택) true = 조회 불능(0명과 구분). */
    bool LoadFromDb( bool *pbUnavailable = nullptr );
    bool Remove( std::string strUserId );
    bool ReloadFromDb( std::string strUserId );

private:
    CSP_USER_MAP m_clsMap;
    CSipMutex m_clsMutex;
    bool _loadUserFromFile( std::string strUserId, CspUser &clsUser );

    bool _remove( std::string strUserId );
    bool _update( CspUser &clsUser );
};

extern CspUserMap gclsCspUserMap;

#endif
