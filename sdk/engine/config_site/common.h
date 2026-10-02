/* CIMS 단말 엔진(ext/pjproject) config_site — 세 플랫폼(Android/Windows/Linux) 공통 결정.
 *
 * 플랫폼 파일(android.h/windows.h/linux.h)이 자기 프렐류드(PJ_CONFIG_* + config_site_sample.h)
 * 뒤에 이 파일을 include 한다. 여기 있는 값은 플랫폼이 다르다고 달라지면 안 되는 것만 둔다 —
 * 정본: docs/design/features/ue_sdk.md §3.
 */
#ifndef CIMS_CONFIG_SITE_COMMON_H
#define CIMS_CONFIG_SITE_COMMON_H

/* 내장 SW 음성코덱 최소화 — 협상 표면 축소. G.711 은 디버그/상호운용 안전망으로 유지.
   음성 정본은 AMR-WB (호시험 표준 코덱) — 백엔드(And-Media / opencore)는 플랫폼 파일이 정한다. */
#define PJMEDIA_HAS_G711_CODEC   1
#define PJMEDIA_HAS_L16_CODEC    0
#define PJMEDIA_HAS_GSM_CODEC    0
#define PJMEDIA_HAS_SPEEX_CODEC  0   /* 코덱만 off. AEC(PJMEDIA_HAS_SPEEX_AEC)는 별개 */
#define PJMEDIA_HAS_ILBC_CODEC   0
#define PJMEDIA_HAS_G722_CODEC   0

/* 시그널링 TLS (sip_tls_signaling.md §7) + 미디어 SRTP SDES e2ae (media_security.md §7).
   빌드만 활성 — 런타임은 계정 정책(프로비저닝 sip_transport / media_srtp)이 켠다. */
#define PJMEDIA_HAS_SRTP          1
#define PJSIP_HAS_TLS_TRANSPORT   1

/* NAT: RTP keepalive(empty RTP) — 청취 전용(무송신) 구간에도 주기 송신해 하향 NAT 매핑·CMP latch
   유지 (ue_nat_traversal.md §7.1). 주기 = PJMEDIA_STREAM_KA_INTERVAL(기본 5s). */
#define PJMEDIA_STREAM_ENABLE_KA  1

/* SIP 메시지 상한 — 수신 rdata 버퍼·송신 tdata 버퍼·TLS 소켓 버퍼의 크기. 기본 4000B 는 서버발 메시지가
   넘는다: 12인 그룹의 prearranged INVITE(mcptt-info + resource-lists + SDP 멀티파트)가 4.5KB 라 UDP 수신이
   4000B 에서 잘리고 마지막 파트인 SDP 가 사라져 호가 미디어 없이 성립했다(TCP/TLS 는 한 메시지가 버퍼를
   넘으면 연결 오류). 등록 transport 가 UDP 인 단말에는 서버가 TCP 로 바꿔 보낼 수 없으므로(NAT) 단말이
   UDP 데이터그램 최대 크기를 받는다. 대가는 rdata·tdata 마다 이 크기의 버퍼(단말 한 대에 수십 개). */
#define PJSIP_MAX_PKT_LEN         65535

/* pjsua2 SdpSession.wholeSdp 인쇄 버퍼 — 기본 1024B 는 SRTP(SDES) 오퍼(RTP m= 라인마다 a=crypto
   전 수트)가 넘친다. 넘치면 wholeSdp="" → 앱 SDP 주입이 조각 SDP 를 만들어 pjmedia_sdp_validate
   assert (media_security.md §7). SDP 한 벌만 담으므로 SIP 메시지 상한과는 별개. */
#define PJSUA2_MAX_SDP_BUF_LEN    4000

/* 음성 패킷 크기: 오디오 SDP 에 a=ptime:20 · a=maxptime:240 — 받을 때 RTP 패킷당 음성 프레임 하나를 요청하고
   12 프레임까지 받는다(GSMA IR.92 §3.2.5 Note 1). pjmedia 는 원래 ptime 을 싣지 않는다 — CIMS 패치
   (pjmedia/src/pjmedia/endpoint.c). 보내는 쪽 크기는 상대 ptime 을 따르고(stream_info.c), 상대가 싣지 않으면
   코어 코덱 정책의 frmPerPkt(AMR-WB 1)가 정한다(ue_sdk.md §4.5). */
#define PJMEDIA_CIMS_SDP_PTIME    20
#define PJMEDIA_CIMS_SDP_MAXPTIME 240

/* 호 품질 측정: RTCP-XR(RFC 3611 VoIP Metrics — 손실/폐기율·버스트/갭·RTT·단말 지연) 생성·수신 통계.
   코어 quality/ 의 E-model 입력이자 상대에게 보내는 XR 보고 (ue_voice_quality.md §3.4). SRTP 호에서는 SRTCP 로 보호된다. */
#define PJMEDIA_HAS_RTCP_XR       1
#define PJMEDIA_STREAM_ENABLE_XR  1

/* 음성 레벨: 마이크 AGC 는 conference bridge 하나(PJMEDIA_CONF_CIMS_MIC_AGC, 목표 -26 dBov)로 통일한다
   (ue_audio_level.md). Speex AEC 전처리의 AGC(기본 켬, 목표 ≈ -12 dBov)가 앞단에서 또 돌면 두 AGC 가
   서로 다른 목표로 싸운다 — Speex AEC 를 쓰는 플랫폼(Windows·Linux)에서 끈다. Android 는 Speex AEC 가
   빠져 있고(config_site_sample) WebRTC AEC 경로에는 AGC 가 없다. */
#define PJMEDIA_SPEEX_AEC_USE_AGC 0

#endif /* CIMS_CONFIG_SITE_COMMON_H */
