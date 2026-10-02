/*
 * MCData 공용 게이트/배포대상/보관 헬퍼 — C-plane(McDataAsModule)과
 * media plane(McDataMediaService)이 공유한다 (TS 24.282 controlling function 검사).
 */

#ifndef _MCDATA_GATES_H_
#define _MCDATA_GATES_H_

#include <string>
#include <vector>

#include "CspPttGroup.h"
#include "McDataCodec.h"

/**
 * @brief 그룹 게이트 검사 — allow_sds/allow_fd(TS 24.481) + 발신자 멤버십 + 발신자 제휴(TS 24.282 §9.2.2.4.2 6)j) ·
 *        §9.2.3.4.4 7)g) · §10.2.4.4.2 12)g) — 미제휴 403 Warning 120). 제휴를 쓰는 그룹(require_affiliation)에서만
 * 제휴를 보고(그 밖의 그룹은 멤버십이 곧 제휴), 제휴 저장소(DB)에 닿지 못하면 판정할 수 없어 500(§9.2.2.4.2 1)).
 * @param piWarn [out, 선택] 거부에 실을 Warning 코드(없으면 0) — 문구는 McDataWarnText
 * @return 0=통과, 아니면 거부할 SIP 상태코드
 */
int McDataGateCheck( const CspPttGroup &clsGroup, const char *pszFrom, bool bFd, int *piWarn = nullptr );

/**
 * @brief 배포 대상 멤버 목록 (§6.3.4 — 제휴 멤버에게만) — 발신자 제외, require_affiliation 그룹은 제휴 멤버만.
 * @return 0 = 대상 있음 · 403 + Warning 198 = 제휴 멤버가 없다(§9.2.2.4.2 6)k)ii) · §9.2.3.4.4 7)i) · §10.2.4.4.2
 * 12)i)) · 500 = 제휴 저장소(DB)에 닿지 못해 대상을 정할 수 없다(§9.2.2.4.2 1) — 제휴하지 않은 멤버에게 보내지 않는다)
 */
int McDataDeliveryTargets( const CspPttGroup &clsGroup, const char *pszFrom, const char *pszGroup,
                           std::vector<std::string> &vecTargets, int *piWarn = nullptr );

/**
 * @brief FD 요청 Payload 검사 (TS 24.282 §10.2.4.4.2 6)·7)) — Payload IE 는 하나(403 210), 내용은 FILEURL(403 211), 그
 * URL 은 이 서버의 media storage function 파일(McDataFdUrlIsOurs — 아니면 403 212). base = Setup.McData.FdUrlBase 또는
 * CSC 단말용 서비스 URL(FD URL 을 만드는 곳과 같은 값).
 * @return 0=통과, 아니면 403 과 *piWarn
 */
int McDataFdPayloadCheck( const CMcDataSdsInfo &clsInfo, int *piWarn );

/** MCData Warning 코드의 문구 (TS 24.282 §4.9 표) — 이 파일의 게이트가 쓰는 것만. 모르면 "". */
const char *McDataWarnText( int iWarn );

/**
 * @brief 그룹 이벤트 + 메시지 보관 기록 (events.jsonl `message_sent` + messages.jsonl).
 *        C-plane/media plane 공용 — media plane 은 pszVia="msrp" 로 구분 필드를 남긴다.
 */
void McDataArchiveMessage( const char *pszGroup, const char *pszFrom, const char *pszMsgType,
                           const CMcDataSdsInfo &clsInfo, int iPayloadSize, int iFanout, const char *pszVia = "",
                           const char *pszFileUrl = "", bool bMcData = true );

/**
 * @brief SDS 발신 기록 — disposition 통지 상관(TS 24.282 §12.2.3 4)·5)) 용 인메모리 색인. 대화·메시지 ID →
 *        원 발신자·그룹(1:1 이면 빈 값). 최근 24 시간·최대 20000 건(오래된 것부터 버린다 — CSP 재기동 전 발신분은 상관
 * 불가). 그룹 SDS(C-plane·media plane)는 McDataArchiveMessage 가, 1:1 SDS 는 디스패처 전달 경로가 부른다.
 */
void McDataRememberSds( const std::string &strConvId, const std::string &strMsgId, const std::string &strSender,
                        const std::string &strGroup );

/** 상관 조회 — 찾으면 true 와 원 발신자·그룹. */
bool McDataCorrelateSds( const std::string &strConvId, const std::string &strMsgId, std::string &strSender,
                         std::string &strGroup );

#endif
