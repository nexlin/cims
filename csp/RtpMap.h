#ifndef _RTP_MAP_H_
#define _RTP_MAP_H_

// RTP 미디어당 소켓 개수 상수.
// (구 CRtpMap/CRtpInfo/CRtpThread 는 CSP 가 RTP 를 직접 relay 하던 미디어서버 분리 전 코드로,
//  멀티 미디어노드 도입 후 포트단독키 충돌 누수 버그의 원인이 되어 제거됨. relay 세션 bookkeeping 은
//  CallMap(CCallInfo) 의 relay descriptor + CmpClient 직접 호출로 대체. 이 상수만 공용으로 유지.)
// 미디어(m-line) 간 포트 간격 — CMP leg 블록 배치(audio=Q, rtcp=Q+1, video=Q+2, video rtcp=Q+3)와
// 일치해야 SDP 의 video m-line 포트가 CMP 수신 소켓과 맞는다. (구 4 는 video 를 Q+4 로 광고해
// CMP(Q+2)와 어긋나던 잠복 결함.)
#define SOCKET_COUNT_PER_MEDIA 2

#endif
