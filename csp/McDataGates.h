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

#endif
