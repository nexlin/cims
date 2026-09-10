#ifndef _CSP_ROLE_H_
#define _CSP_ROLE_H_

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

/**
 * @ingroup CspServer
 * @brief 역할 (mcptt_authorization.md §2.2) — CSP 가 SIP 경로 인가에서 읽는 필드만 든다.
 *
 * 권한 = 능력 + 범위. 콘솔 프리셋(admin/manager/operator/monitor, 전역)과 관제 프리셋(감독/관리/전체, 한정)이 같은
 * roles 행이지만 CSP 는 SIP 신원이 있는 배정(role_assignments.principal_type='user')만 적재한다. 관리 범위
 * (directory_write)는 CSC 만 읽는다.
 */
class CspRole {
public:
    std::string m_strId;
    std::string m_strName;
    std::string m_strMonitorCall;       ///< none(기본) | own | listed | all — 통화 감청·세션 관측(§5.2 규칙 2)
    std::string m_strPttListen;         ///< none(기본) | listed | all — PTT 청취·conference 구독(§5.6)
    std::string m_strListenVisibility;  ///< hidden(기본) | visible — 청취 로스터 노출
    /** monitor_call=listed 의 감시 대상 전화 그룹 id */
    std::set<std::string> m_setMonitorTargets;
    /** ptt_listen=listed 의 청취 대상 PTT 그룹 (mcptt_group_id) */
    std::set<std::string> m_setPttTargets;
    /** 배정된 회선 id (volte·ptt 전 회선으로 펼친 것) — JSON fallback 의 assignments[] / DB 적재 시 person → 회선 확장
     */
    std::vector<std::string> m_vecLines;

    CspRole() {
        Clear();
    }
    void Clear();

    /** JSON fallback 파일(DataFolder.Role/<id>.json) 로드 — id 는 파일명. */
    bool LoadFile( const std::string &strPath );
};

/**
 * @ingroup CspServer
 * @brief 역할 인메모리 맵 (dispatch_center.md §3.5) — 역할 id 인덱스 + 회선 → 역할 인덱스.
 *
 * 적재 시 role_assignments(principal_type='user') 를 그 person 의 전 회선(volte·ptt) id 로 펼쳐 SIP 신원으로 바로
 * 묻는다 — PTT 회선의 청취 인가와 유선 회선의 감청 인가가 같은 사람의 역할을 본다. ROLE_CHANGED·USER_CHANGED
 * (회선 개설/삭제)·CSC_RESTART 로 재적재. 판정 = CanWatch(§5.2)·CanListenPtt(§5.6)·ListenHidden.
 */
class CCspRoleMap {
public:
    bool LoadFromDb();
    bool Load( const char *pszDirName );

    void Insert( const CspRole &clsRole );
    void Remove( const char *pszRoleId );
    void Clear();

    bool Select( const char *pszRoleId, CspRole &clsRole );
    /** 회선(SIP 신원)의 역할 — 배정 없으면 false. */
    bool SelectForLine( const char *pszLineId, CspRole &clsRole );
    /** 회선의 역할 id — 배정 없으면 빈 값 (감사 E-AUD-016 `role` 필드). */
    std::string RoleIdForLine( const char *pszLineId );

    /** dialog 이벤트·Join 인가 (dispatch_center.md §5.2) — 질문 둘:
     *  규칙 1(전화 그룹) 같은 비어 있지 않은 그룹 → 허용 / 규칙 2(역할) watcher 회선의 monitor_call 로 판정.
     *  strTargetGroup = 대상의 전화 그룹 id (대상이 대표번호면 그 그룹). */
    bool CanWatch( const char *pszWatcherLine, const std::string &strTargetGroup );
    /** PTT 그룹콜 청취 범위 (§5.6 ptt_listen) — 청취자 회선의 역할이 pszPttGroupId 를 들을 수 있는가. */
    bool CanListenPtt( const char *pszListenerLine, const char *pszPttGroupId );
    /** 청취 로스터 은닉 여부 — 역할 listen_visibility != visible (역할 없음 = hidden). */
    bool ListenHidden( const char *pszListenerLine );

    int GetCount();

private:
    std::map<std::string, CspRole> m_clsMap;
    std::map<std::string, std::string> m_clsLineIndex;  ///< line id → role id
    std::recursive_mutex m_clsMutex;

    void _index( const CspRole &clsRole );
    void _unindex( const CspRole &clsRole );
};

extern CCspRoleMap gclsRoleMap;

#endif
