#ifndef _RTP_MAP_H_
#define _RTP_MAP_H_

// RTP 미디어당 소켓 개수 상수.
// (구 CRtpMap/CRtpInfo/CRtpThread 는 CSP 가 RTP 를 직접 relay 하던 미디어서버 분리 전 코드로,
//  멀티 미디어노드 도입 후 포트단독키 충돌 누수 버그의 원인이 되어 제거됨. relay 세션 bookkeeping 은
//  CallMap(CCallInfo) 의 relay descriptor + CmpClient 직접 호출로 대체. 이 상수만 공용으로 유지.)
#define SOCKET_COUNT_PER_MEDIA 4

#endif
