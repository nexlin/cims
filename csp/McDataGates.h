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
 * @brief 그룹 게이트 검사 — allow_sds/allow_fd(TS 24.481) + 발신자 멤버십.
 * @return 0=통과, 아니면 거부할 SIP 상태코드(403)
 */
int McDataGateCheck( const CspPttGroup &clsGroup, const char *pszFrom, bool bFd );

/** 배포 대상 멤버 목록 — 발신자 제외, require_affiliation 그룹은 affiliate 멤버만 */
void McDataDeliveryTargets( const CspPttGroup &clsGroup, const char *pszFrom, const char *pszGroup,
                            std::vector<std::string> &vecTargets );

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
