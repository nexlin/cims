// libcimsue — C API (ue_sdk.md §6.4)
//
// 공개 C++ 헤더(types.h·listener.h·engine.h·csc.h)를 손으로 1:1 평탄화한 층이다. 같은 cimsue.dll 이
// C++ 클래스와 함께 export 하며, .NET 파사드(CimsUe.dll)는 P/Invoke 로 이 표면만 본다.
// C++ 공개 헤더가 바인딩 정본이라는 규칙은 Android(SWIG) 와 같다 — 새 C++ API 는 같은 변경에서 여기에도
// 반영하고, 이름·인자 순서는 원본 헤더를 그대로 따른다.
//
// 규약
//   - 핸들: 불투명 포인터(cimsue_engine_t*·cimsue_csc_t*). 계정·호·라우트는 코어와 같은 정수 id.
//   - 명령: cimsue_status_t 동기 반환 — 0=성공, 음수=코어 오류, 양수=pjsua/HTTP 상태(C++ Result::code 그대로).
//           실패 사유 문자열은 cimsue_last_error() (스레드별, 같은 스레드의 다음 실패 전까지 유효).
//   - id 를 돌려주는 함수는 >=0 이 id, -1 이 실패(사유는 마찬가지로 cimsue_last_error()).
//   - 문자열은 UTF-8. 코어가 소유한 문자열·배열의 수명은 둘 중 하나다.
//       * 콜백 인자        → 그 콜백이 반환할 때까지
//       * 조회(getter) 산출 → 같은 스레드가 다음 조회를 부를 때까지 (스레드별 스냅샷)
//     더 오래 쓰려면 복사한다. 입력 문자열은 호출이 반환할 때까지만 읽는다(코어가 보관하지 않는다).
//   - 이벤트: cimsue_listener_t — Listener 가상함수 1:1 의 함수 포인터 한 벌 + void* user. NULL 이면 무시.
//             콜백은 코어 이벤트 스레드에서 오며, 그 안에서 다시 명령을 불러도 교착하지 않는다.
//   - 구조체는 POD, 가변 배열은 (ptr, count) 쌍, 참/거짓은 int32_t(0/1). 열거형 값은 C++ 과 같다.
//   - 입력 설정 구조체는 cimsue_*_default() 로 채운 뒤 필요한 필드만 덮어쓴다(C++ 기본값이 정본 —
//     default() 는 문자열 필드를 NULL 로 두고, NULL 은 "C++ 기본값 유지" 로 읽힌다. 빈 문자열은 지운다).
#ifndef CIMSUE_C_H
#define CIMSUE_C_H

#include <stdint.h>

#include "cimsue/export.h"

#if defined(_WIN32)
#  define CIMSUE_CALL __cdecl
#else
#  define CIMSUE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cimsue_engine cimsue_engine_t;
typedef struct cimsue_csc cimsue_csc_t;

/** 명령의 즉시 결과(C++ Result::code). 0 = 성공. */
typedef int32_t cimsue_status_t;
#define CIMSUE_OK 0

/* ── 열거형 (types.h 와 같은 값) ── */

typedef enum { CIMSUE_TRANSPORT_UDP = 0, CIMSUE_TRANSPORT_TCP = 1, CIMSUE_TRANSPORT_TLS = 2 } cimsue_transport_t;
typedef enum { CIMSUE_AUTH_DIGEST = 0, CIMSUE_AUTH_AKA = 1 } cimsue_auth_scheme_t;
typedef enum { CIMSUE_SRTP_OFF = 0, CIMSUE_SRTP_OPTIONAL = 1, CIMSUE_SRTP_REQUIRED = 2 } cimsue_media_security_t;
typedef enum {
    CIMSUE_REG_UNREGISTERED = 0, CIMSUE_REG_REGISTERING = 1, CIMSUE_REG_REGISTERED = 2, CIMSUE_REG_FAILED = 3
} cimsue_reg_state_t;
typedef enum {
    CIMSUE_CALL_NULL = 0, CIMSUE_CALL_OUTGOING = 1, CIMSUE_CALL_INCOMING = 2,
    CIMSUE_CALL_ACTIVE = 3, CIMSUE_CALL_HELD = 4, CIMSUE_CALL_DISCONNECTED = 5
} cimsue_call_state_t;
typedef enum { CIMSUE_DIR_OUTGOING = 0, CIMSUE_DIR_INCOMING = 1 } cimsue_call_dir_t;
typedef enum {
    CIMSUE_FLOOR_IDLE = 0, CIMSUE_FLOOR_REQUESTING = 1, CIMSUE_FLOOR_SPEAKING = 2,
    CIMSUE_FLOOR_LISTENING = 3, CIMSUE_FLOOR_QUEUED = 4
} cimsue_floor_state_t;
typedef enum {
    CIMSUE_FLOOR_EV_GRANTED = 0, CIMSUE_FLOOR_EV_DENIED = 1, CIMSUE_FLOOR_EV_IDLE = 2,
    CIMSUE_FLOOR_EV_TAKEN = 3, CIMSUE_FLOOR_EV_TALKER_LEFT = 4, CIMSUE_FLOOR_EV_REVOKED = 5,
    CIMSUE_FLOOR_EV_QUEUE_POSITION = 6, CIMSUE_FLOOR_EV_QUEUE_CANCELLED = 7,
    CIMSUE_FLOOR_EV_REQUEST_TIMEOUT = 8, CIMSUE_FLOOR_EV_TALK_LIMIT = 9, CIMSUE_FLOOR_EV_OTHER = 10
} cimsue_floor_kind_t;
/** on_mcptt_condition 의 계기(types.h ConditionCause). */
typedef enum {
    CIMSUE_COND_LOCAL = 0, CIMSUE_COND_CONFIRMED = 1, CIMSUE_COND_DENIED = 2, CIMSUE_COND_ADVERTISED = 3
} cimsue_condition_cause_t;
/** MC 서비스(types.h McService) — 호·affiliation 은 서비스마다 따로다(TS 23.280 §5.2.5). */
typedef enum { CIMSUE_MC_SERVICE_MCPTT = 0, CIMSUE_MC_SERVICE_MCVIDEO = 1 } cimsue_mc_service_t;
/** MCVideo 내 송출 상태(types.h TransmissionState — TS 24.581 §6.2.4 'U: …'). */
typedef enum {
    CIMSUE_TX_NO_PERMISSION = 0, CIMSUE_TX_PENDING_REQUEST = 1, CIMSUE_TX_PERMITTED = 2, CIMSUE_TX_PENDING_END = 3, CIMSUE_TX_QUEUED = 4
} cimsue_transmission_state_t;
/** 한 송출의 내 수신 상태(types.h ReceptionState — §6.2.5). */
typedef enum {
    CIMSUE_RX_NOTIFIED = 0, CIMSUE_RX_PENDING_REQUEST = 1, CIMSUE_RX_RECEIVING = 2, CIMSUE_RX_PENDING_RELEASE = 3, CIMSUE_RX_ENDED = 4
} cimsue_reception_state_t;
/** on_transmission 의 종류(types.h TransmissionEvent::Kind). */
typedef enum {
    CIMSUE_TXEV_GRANTED = 0, CIMSUE_TXEV_REJECTED = 1, CIMSUE_TXEV_REVOKED = 2, CIMSUE_TXEV_QUEUE_POSITION = 3, CIMSUE_TXEV_END_REQUESTED = 4,
    CIMSUE_TXEV_ENDED = 5, CIMSUE_TXEV_RECEIVER_JOINED = 6, CIMSUE_TXEV_IDLE = 7, CIMSUE_TXEV_QUEUE_CANCELLED = 8, CIMSUE_TXEV_REQUEST_TIMEOUT = 9,
    CIMSUE_TXEV_OTHER = 10
} cimsue_transmission_kind_t;
/** on_reception 의 종류(types.h ReceptionEvent::Kind). */
typedef enum {
    CIMSUE_RXEV_NOTIFIED = 0, CIMSUE_RXEV_GRANTED = 1, CIMSUE_RXEV_REJECTED = 2, CIMSUE_RXEV_ENDED = 3, CIMSUE_RXEV_RELEASED = 4,
    CIMSUE_RXEV_END_REQUESTED = 5, CIMSUE_RXEV_REQUEST_TIMEOUT = 6, CIMSUE_RXEV_OTHER = 7
} cimsue_reception_kind_t;
/** 오디오 라우트(types.h AudioRoute) — 입력의 EARPIECE = 내장 기본 마이크 고정, DEFAULT = 정책. */
typedef enum { CIMSUE_ROUTE_DEFAULT = 0, CIMSUE_ROUTE_EARPIECE = 1, CIMSUE_ROUTE_LOUDSPEAKER = 2 } cimsue_audio_route_t;
/** 마이크 AGC 기본 목표(types.h kMicAgcTargetDbov) — ITU-T P.56 활성 레벨 -26 dBov. */
#define CIMSUE_MIC_AGC_TARGET_DBOV (-26.0)

/* ── 설정 구조체 (입력) ── */

typedef struct {
    const char* user_agent;
    int32_t     log_level;              /* pjsip 로그 레벨 0~6 → on_log */
    const char* tls_ca_pem;             /* SIP TLS·HTTPS 공용 신뢰 앵커(PEM). NULL = 시스템 기본 */
    int32_t     tls_verify_server;
    int32_t     null_audio_device;      /* 헤드리스(장치 없이) */
    int32_t     no_vad;
    int32_t     udp_port, tcp_port, tls_port;   /* 0 = 임의 포트 */
    uint32_t    clock_rate;
    /* 끝에 덧붙였다 */
    int32_t     udp_no_tcp_switch;      /* RFC 3261 §18.1.1 UDP→TCP 승격 비활성(통제된 망 사이트 옵션 — ServiceProfile.udp_no_tcp_switch) */
    int32_t     grant_mic_delay_ms;     /* Floor Granted 뒤 마이크를 여는 지연(승인 톤 길이, 0 = 즉시) */
} cimsue_engine_config_t;

typedef struct {
    const char*             server_host;
    int32_t                 server_port;
    cimsue_transport_t      transport;
    const char*             domain;
    const char*             msisdn;
    const char*             imsi;
    const char*             auth_id;        /* 전체 IMPI 직접 지정. NULL 이면 imsi@domain 합성 */
    const char*             display_name;
    const char*             ha1;            /* MD5(IMPI:realm:pw) hex32 — 평문보다 우선 */
    const char*             password;
    cimsue_auth_scheme_t    auth_scheme;
    const char*             aka_k;
    const char*             aka_opc;
    const char*             aka_amf;
    const char* const*      sec_mechanisms; /* RFC 3329 목록 (ptr, count) */
    int32_t                 sec_mechanism_count;
    cimsue_media_security_t media_security;
    int32_t                 expires_sec;
    const char*             contact_params;
    int32_t                 video_auto_transmit;
    const char*             mcptt_id;       /* 비면 "tel:"+msisdn */
    int32_t                 auto_answer_mcptt;
    const char*             instance_id;    /* REGISTER +sip.instance URN(꺾쇠 없이) — NULL 이면 pjsip 기본값 */
    /* 끝에 덧붙였다(types.h AccountConfig 의 MCPTT·MCData 필드) */
    const char*             mcptt_client_id;    /* TS 24.379 §4.10 — NULL·빈 값이면 instance_id 가 urn:uuid: 일 때 그것 */
    const char*             rp_emergency;       /* Resource-Priority r-value — NULL = C++ 기본(mcpttp.15) */
    const char*             rp_imminent_peril;  /* NULL = mcpttp.8 */
    const char*             rp_normal;          /* NULL = mcpttp.0 */
    int32_t                 max_sds_cplane_bytes; /* 그룹 SDS 시그널링 평면 상한 — 넘으면 MSRP(TS 24.282 §9.2.3). 0 = 제한 없음 */
    int32_t                 mcdata_msrp;        /* 서버발 MSRP 배포 수신(REGISTER Contact ICSI mcdata.sds) */
    const char*             mcptt_server_uri;   /* 참여 MCPTT 기능 PSI — 긴급 경보 Request-URI(TS 24.379 §12.1.1.1 8)) */
    const char*             mcdata_server_uri;  /* 참여 MCData 기능 PSI — SDS disposition 통지 Request-URI(TS 24.282 §12.2.1.1). NULL·빈 값 = 원 발신자 직행 */
    int32_t                 mcvideo_enabled;    /* REGISTER Contact 에 MCVideo 태그(TS 24.281 §7.2.1AA) — 빼면 MCVideo 로그오프 */
    const char*             mcvideo_server_uri; /* 참여 MCVideo 기능 PSI — MCVideo 그룹 호·affiliation Request-URI(§9.2.1.2.1.1·§8.2) */
    int32_t                 auto_answer_mcvideo; /* MCVideo 멤버 초대 자동 수락(§6.2.3.1.2) — 기본 1 */
} cimsue_account_config_t;

typedef struct {
    int32_t video;
    int32_t emergency;
} cimsue_call_options_t;

typedef struct {
    int32_t            emergency;
    int32_t            imminent_peril;
    int32_t            listen_only;     /* a=recvonly 청취 합류 — floor 요청 불가 */
    int32_t            full_duplex;     /* mc_no_floor_ctrl — start_private_call 전용 */
    const char* const* members;         /* 애드혹 임시 그룹 멤버(tel: URI) — join_group_call 전용 */
    int32_t            member_count;
    int32_t            broadcast;       /* 일제 통화 개시(<broadcast-ind>true, TS 24.379 §4.12) — join_group_call 전용 */
    int32_t            implicit_floor_request; /* 암묵적 발언 요청(mc_implicit_request+mc_granted, TS 24.380 §14.2.4·§14.2.5) */
} cimsue_group_call_options_t;

/** MCVideo 그룹 호 개시·합류 옵션(types.h VideoGroupCallOptions — TS 24.281 §9.2.1·§9.2.2, fmtp TS 24.581 §14.2). */
typedef struct {
    int32_t     prearranged;                    /* session-type prearranged(0 = chat) */
    int32_t     queueing;                       /* mc_queueing */
    int32_t     max_priority;                   /* mc_priority 1~255, <0 = 미기재 */
    int32_t     max_reception_priority;         /* mc_reception_priority 1~255, <0 = 미기재 */
    int32_t     implicit_transmission_request;  /* mc_implicit_request + mc_granted */
    const char* session_uri;                    /* 재합류(§9.2.1.2.4) — 앞 호의 call_info.session_uri. NULL = 새 합류 */
} cimsue_video_group_call_options_t;

/** send_request 의 부가 헤더. */
typedef struct {
    const char* name;
    const char* value;
} cimsue_header_t;

/* ── 상태·이벤트 구조체 (산출) ── */

typedef struct {
    int32_t            account_id;
    cimsue_reg_state_t state;
    int32_t            code;
    const char*        reason;
    int32_t            expires_sec;
} cimsue_reg_info_t;

typedef struct {
    int32_t     present;
    const char* session_type;           /* prearranged/chat/private/... (TS 24.379 Annex F.1) */
    const char* request_uri;
    const char* calling_user_id;
    const char* calling_group_id;
    int32_t     emergency;
    int32_t     imminent_peril;
    int32_t     private_call;
    int32_t     no_floor_ctrl;
    int32_t     broadcast;              /* <broadcast-ind> — 일제 통화 */
} cimsue_mcptt_info_t;

/** MCPTT 세션 조건(types.h McpttCondition) — 긴급·임박의 현재값. 긴급이 임박을 대체한다. */
typedef struct {
    int32_t emergency;
    int32_t imminent_peril;
    int32_t mine;                       /* 이 단말이 올린 조건 */
    int32_t pending;                    /* 상향·하향 re-INVITE 응답 대기 */
    int32_t last_code;                  /* 마지막 상향·하향 re-INVITE 의 최종 응답 */
} cimsue_mcptt_condition_t;

/** 긴급 경보·긴급 통지 수신(types.h EmergencyAlert, TS 24.379 §12.1.1.3). 지시자 1 = true, -1 = false, 0 = 요소 없음. */
typedef struct {
    int32_t     account_id;
    const char* group_id;               /* bare */
    const char* user_id;                /* bare 발신자 */
    const char* originated_by;          /* bare — 제3자 취소가 가리키는 원 경보 발신자 */
    const char* mc_org;
    int32_t     alert_ind;              /* 1 경보 · -1 경보 취소 · 0 그룹 상태 통지 */
    int32_t     emergency_ind;
    int32_t     imminent_peril_ind;
    int32_t     self;                   /* 발신자가 이 계정(에코) */
} cimsue_emergency_alert_t;

/** 한 호 안의 RTP 소스(SSRC) — U10 디먹스 산출. */
typedef struct {
    uint32_t    ssrc;
    const char* label;
    int32_t     active;
    float       level;
} cimsue_media_source_t;

typedef struct {
    int32_t                      call_id;
    int32_t                      account_id;
    cimsue_call_dir_t            dir;
    cimsue_call_state_t          state;
    const char*                  remote_uri;
    const char*                  called_party;      /* P-Called-Party-ID — 대표번호 착신 식별 */
    int32_t                      video;
    int32_t                      media_active;
    int32_t                      muted;
    int32_t                      listen;
    int32_t                      playback_route;
    int32_t                      last_code;
    const char*                  last_reason;
    const cimsue_media_source_t* sources;
    int32_t                      source_count;
    int32_t                      is_mcptt;
    const char*                  group_id;
    cimsue_mcptt_info_t          mcptt;
    int32_t                      half_duplex;
    int32_t                      listen_only;
    const char*                  joined_dialog;     /* INVITE-Join 대상 dialog 의 Call-ID */
    /* 끝에 덧붙였다 */
    float                        rx_level;          /* 이 호에서 듣는 크기(set_rx_level — 코어가 기억해 재결선마다 적용) */
    cimsue_mcptt_condition_t     condition;         /* 세션 조건 현재값 — mcptt 는 개시·착신 INVITE 의 값(불변) */
    const char*                  answer_state;      /* 개시 200 OK 의 P-Answer-State(RFC 4964) — "Unconfirmed" = 멤버 확인 전 수락 */
    const char* const*           non_ack_users;     /* 서버가 알린 미응답 멤버 MCPTT ID(bare, TS 24.379 §6.3.3.3) (ptr, count) */
    int32_t                      non_ack_user_count;
    cimsue_mc_service_t          service;           /* MCVideo 그룹 호면 MCVIDEO(그때 is_mcptt = 0) */
    const char*                  session_uri;       /* MC 세션 식별자 — 제어 기능 Contact(isfocus), 재합류에 쓴다 */
    int32_t                      video_send;        /* 내 영상 송출 허용(set_video_send, 기본 1) — MCPTT 반이중은 발언권을 가진 동안만 실제로 보낸다 */
} cimsue_call_info_t;

typedef struct {
    const char* id;                     /* MCPTT ID */
    uint32_t    ssrc;                   /* 0 = 미상 */
    int32_t     self;
} cimsue_talker_t;

typedef struct {
    cimsue_floor_kind_t    kind;
    int32_t                call_id;
    cimsue_floor_state_t   state;
    int32_t                duration_sec;
    int32_t                cause;
    const char*            cause_text;
    int32_t                indicator;
    int32_t                permission;
    int32_t                queue_position;
    int32_t                me_speaking;
    const cimsue_talker_t* talkers;
    int32_t                talker_count;
    int32_t                raw_type;
} cimsue_floor_event_t;

typedef struct {
    cimsue_floor_state_t   state;
    const cimsue_talker_t* talkers;
    int32_t                talker_count;
    int32_t                can_request;
    int32_t                indicator;
    int32_t                queue_position;
    int32_t                local_port;
    const char*            remote_ip;
    int32_t                remote_port;
    uint32_t               granted_count, taken_count, deny_count;
} cimsue_floor_info_t;

/** 한 송출(types.h VideoTransmitter — Media Transmission Notification §9.2.13). */
typedef struct {
    const char*              user_id;          /* 송출자 MCVideo ID(서버 표기) — accept_reception·end_reception 인자 */
    uint32_t                 audio_ssrc;
    uint32_t                 video_ssrc;
    const char*              functional_alias;
    int32_t                  automatic;        /* Reception Mode 0 — 서버가 곧바로 수신 허가 */
    cimsue_reception_state_t state;
} cimsue_video_transmitter_t;

/** 송출 제어 이벤트(types.h TransmissionEvent — TS 24.581 §6.2.4). */
typedef struct {
    cimsue_transmission_kind_t  kind;
    int32_t                     call_id;
    cimsue_transmission_state_t state;
    int32_t                     cause;           /* Rejected·Revoked·EndRequested 의 Reject Cause */
    const char*                 cause_text;
    int32_t                     duration_sec;
    int32_t                     priority;
    int32_t                     queue_position;
    int32_t                     indicator;
    uint32_t                    audio_ssrc, video_ssrc;
    const char*                 receiver_id;     /* ReceiverJoined */
    int32_t                     raw_type;
} cimsue_transmission_event_t;

/** 수신 제어 이벤트(types.h ReceptionEvent — §6.2.5). */
typedef struct {
    cimsue_reception_kind_t    kind;
    int32_t                    call_id;
    cimsue_video_transmitter_t transmitter;
    int32_t                    cause;
    const char*                cause_text;
    int32_t                    raw_type;
} cimsue_reception_event_t;

/** MCVideo 호의 전송 제어 현재값(types.h TransmissionInfo). */
typedef struct {
    cimsue_transmission_state_t       state;
    const cimsue_video_transmitter_t* transmitters;  /* 알려진 송출(내 것 제외) (ptr, count) */
    int32_t                           transmitter_count;
    int32_t                           queue_position;
    int32_t                           local_port;
    const char*                       remote_ip;
    int32_t                           remote_port;
} cimsue_transmission_info_t;

typedef struct {
    int32_t     account_id;
    int64_t     token;
    const char* method;
    int32_t     code;
    const char* reason;
    const char* etag;                   /* SIP-ETag (PUBLISH) */
} cimsue_request_result_t;

/** 감시 대상의 dialog 상태 (RFC 4235) — Join 대상 식별의 입력. */
typedef struct {
    int32_t     account_id;
    const char* watched;
    const char* id;
    const char* call_id;
    const char* local_tag;
    const char* remote_tag;
    const char* direction;              /* initiator|recipient */
    const char* state;                  /* trying|proceeding|early|confirmed|terminated */
    const char* remote_identity;
    int32_t     full;
} cimsue_dialog_info_t;

typedef struct {
    const char* uri;
    const char* status;
} cimsue_roster_entry_t;

typedef struct {
    int32_t     account_id;
    const char* from_uri;
    const char* group_uri;
    const char* conv_id;
    const char* msg_id;
    int64_t     time_sec;
    int32_t     disposition_req;        /* 0 없음 / 1 delivery / 2 read / 3 both */
    const char* text;
    int32_t     notification;
    int32_t     notif_type;             /* 1 undelivered / 2 delivered / 3 read / 4 delivered+read */
    int32_t     fd;
    const char* file_url;
    const char* file_name;
    const char* file_type;
    int64_t     file_size;
    int32_t     media_plane;            /* media plane(MSRP) 배포로 받았다(TS 24.282 §9.2.3) — 끝에 덧붙였다 */
} cimsue_sds_message_t;

/** MCData FD 로 알릴 파일(types.h FdFile) — send_group_fd/send_fd 입력. url = csc_upload_fd 결과, 문자열 NULL = 빈 값. */
typedef struct {
    const char* url;
    const char* name;
    const char* type;                   /* MIME (NULL·빈 값 = application/octet-stream) */
    int64_t     size;
} cimsue_fd_file_t;

typedef struct {
    uint32_t rx_packets, rx_bytes, rx_loss, rx_discard;
    uint32_t tx_packets, tx_bytes;
    int32_t  valid;
} cimsue_stream_stats_t;

/* 호 품질 한 방향(types.h QualityDirection) — 비율 %, 값 없음 = -1, 레벨 127 = 없음 */
typedef struct {
    int32_t  valid;
    uint32_t packets, lost, discarded;
    double   loss_pct, discard_pct, jitter_ms, jitter_max_ms;
    double   burst_density_pct, gap_density_pct;
    int32_t  burst_ms, gap_ms;
    int32_t  signal_dbm, noise_dbm;
} cimsue_quality_direction_t;

/* 호 품질(types.h CallQuality — ue_voice_quality.md §3). codec 은 같은 스레드의 다음 조회까지 유효 */
typedef struct {
    int32_t                    valid;
    const char*                codec;
    uint32_t                   clock_rate;
    int32_t                    wideband;
    cimsue_quality_direction_t rx;
    cimsue_quality_direction_t remote;
    double                     rtd_ms, esd_ms, one_way_ms;
    double                     r_lq, r_cq, mos_lq, mos_cq;
    int64_t                    start_epoch_ms, duration_ms;
} cimsue_call_quality_t;

typedef struct {
    int32_t     id;
    const char* name;
    const char* driver;
    uint32_t    input_count;
    uint32_t    output_count;
} cimsue_audio_device_info_t;

/** 영상 장치(types.h VideoDeviceInfo). */
typedef struct {
    int32_t     id;
    const char* name;
    const char* driver;
    int32_t     capture;
    int32_t     render;
} cimsue_video_device_info_t;

/** 영상 프레임 한 장(types.h VideoFrame — 창 없는 프레임 렌더 빌드, Windows). BGRA 32 bpp, 위 줄부터. data 는 콜백 동안만 유효. */
typedef struct {
    int32_t        call_id;     /* 수신 영상 = 그 호, -1 = 내 카메라(셀프뷰 — cimsue_engine_set_video_preview) */
    int32_t        width;
    int32_t        height;
    int32_t        stride;      /* 한 줄 바이트 수 */
    const uint8_t* data;
    int64_t        size;        /* stride × height */
} cimsue_video_frame_t;

/** 서버 인증서 만료 관측(types.h TlsPeerExpiry) — 마지막 성공 TLS 핸드셰이크의 peer 인증서. valid=0 이면 관측 없음. */
typedef struct {
    int32_t     valid;
    int64_t     not_after_epoch;        /* UTC epoch 초 */
    int64_t     observed_epoch;         /* 관측 시각 */
    int32_t     days_left;              /* 지금 기준 잔여 일수(음수 = 만료). valid=0 이면 0 */
    const char* subject;                /* peer 인증서 subject(한 줄) */
    const char* remote;                 /* 관측한 상대 host:port */
} cimsue_tls_peer_expiry_t;

/* ── 리스너 (listener.h 1:1) ── */

typedef struct {
    void* user;
    void (CIMSUE_CALL* on_log)(void* user, int32_t level, const char* msg);
    void (CIMSUE_CALL* on_reg_state)(void* user, const cimsue_reg_info_t* info);
    void (CIMSUE_CALL* on_incoming_call)(void* user, const cimsue_call_info_t* info);
    void (CIMSUE_CALL* on_call_state)(void* user, const cimsue_call_info_t* info);
    void (CIMSUE_CALL* on_call_media)(void* user, const cimsue_call_info_t* info);
    void (CIMSUE_CALL* on_floor)(void* user, const cimsue_floor_event_t* ev);
    void (CIMSUE_CALL* on_roster)(void* user, int32_t account_id, const char* group_id,
                                  const cimsue_roster_entry_t* users, int32_t user_count, int32_t full);
    void (CIMSUE_CALL* on_dialog_info)(void* user, const cimsue_dialog_info_t* d);
    void (CIMSUE_CALL* on_sds)(void* user, const cimsue_sds_message_t* msg);
    void (CIMSUE_CALL* on_request_result)(void* user, const cimsue_request_result_t* r);
    void (CIMSUE_CALL* on_message)(void* user, int32_t account_id, const char* from_uri,
                                   const char* content_type, const char* body);
    void (CIMSUE_CALL* on_engine_stopped)(void* user);
    /* 끝에 덧붙였다 */
    /** MCPTT 세션 조건 변화 — info->condition 이 새 값, cause 가 계기(Listener::onMcpttCondition). */
    void (CIMSUE_CALL* on_mcptt_condition)(void* user, const cimsue_call_info_t* info, cimsue_condition_cause_t cause);
    /** 긴급 경보·취소·긴급 통지 수신 — 200 OK 는 코어가 이미 보냈다(Listener::onEmergencyAlert). */
    void (CIMSUE_CALL* on_emergency_alert)(void* user, const cimsue_emergency_alert_t* alert);
    /** 개시 호의 미응답 멤버 알림(INFO g.3gpp.mcptt-info, TS 24.379 §6.3.3.3) — info->non_ack_users(Listener::onNonAcknowledgedUsers). */
    void (CIMSUE_CALL* on_non_acknowledged_users)(void* user, const cimsue_call_info_t* info);
    /** MCVideo 송출 제어(Listener::onTransmission, TS 24.581 §6.2.4) — 송출(마이크·카메라) 게이트는 코어가 이미 처리했다. */
    void (CIMSUE_CALL* on_transmission)(void* user, const cimsue_transmission_event_t* ev);
    /** MCVideo 수신 제어(Listener::onReception, §6.2.5) — 새 송출 알림(manual 이면 앱이 accept_reception)·수신 허가·종료. */
    void (CIMSUE_CALL* on_reception)(void* user, const cimsue_reception_event_t* ev);
    /** 영상 프레임(Listener::onVideoFrame — 프레임 렌더 빌드만). **예외: 이벤트 스레드가 아니라 영상 스레드**에서 프레임마다 곧바로
     *  불린다 — 화소를 복사하고 곧 돌아간다. 이 콜백 안에서 엔진 함수를 부르지 않는다(교착). */
    void (CIMSUE_CALL* on_video_frame)(void* user, const cimsue_video_frame_t* frame);
} cimsue_listener_t;

/* ── 엔진 (engine.h 1:1) ── */

/** 엔진 객체 생성(아직 기동하지 않음). 프로세스당 1개. */
CIMSUE_API cimsue_engine_t* CIMSUE_CALL cimsue_engine_create(void);
/** 기동 중이면 stop 후 파괴. */
CIMSUE_API void CIMSUE_CALL cimsue_engine_destroy(cimsue_engine_t* e);

CIMSUE_API void CIMSUE_CALL cimsue_engine_config_default(cimsue_engine_config_t* cfg);
/** 기동. listener 구조체는 이 호출 안에서 복사한다 — 함수 포인터와 user 는 stop 까지 유효해야 한다.
 *  listener=NULL 이면 이벤트를 받지 않는다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_start(cimsue_engine_t* e, const cimsue_engine_config_t* cfg,
                                                           const cimsue_listener_t* listener);
CIMSUE_API void CIMSUE_CALL cimsue_engine_stop(cimsue_engine_t* e);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_running(const cimsue_engine_t* e);

/* 계정 */
CIMSUE_API void CIMSUE_CALL cimsue_account_config_default(cimsue_account_config_t* cfg);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_add_account(cimsue_engine_t* e, const cimsue_account_config_t* cfg);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_register_account(cimsue_engine_t* e, int32_t account_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_unregister_account(cimsue_engine_t* e, int32_t account_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_refresh_registration(cimsue_engine_t* e, int32_t account_id);
/** 망 변경 — TCP/TLS 연결을 닫고 등록을 켠 계정마다 재등록(Engine::handleNetworkChange). 앞 등록이 걸려 있으면 끝난 뒤 한 번 더. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_handle_network_change(cimsue_engine_t* e);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_remove_account(cimsue_engine_t* e, int32_t account_id);
CIMSUE_API void CIMSUE_CALL cimsue_engine_reg_info(const cimsue_engine_t* e, int32_t account_id,
                                                   cimsue_reg_info_t* out);
/** 계정 id 목록. 반환 개수, *out 은 스냅샷 배열(다음 조회까지 유효). */
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_accounts(const cimsue_engine_t* e, const int32_t** out);

/* 호 (VoLTE 1:1) — opts=NULL 이면 기본값 */
CIMSUE_API void CIMSUE_CALL cimsue_call_options_default(cimsue_call_options_t* opts);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_dial(cimsue_engine_t* e, int32_t account_id, const char* target,
                                                  const cimsue_call_options_t* opts);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_answer(cimsue_engine_t* e, int32_t call_id,
                                                            const cimsue_call_options_t* opts);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_reject(cimsue_engine_t* e, int32_t call_id, int32_t status_code);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_hangup(cimsue_engine_t* e, int32_t call_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_hold(cimsue_engine_t* e, int32_t call_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_resume(cimsue_engine_t* e, int32_t call_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_muted(cimsue_engine_t* e, int32_t call_id, int32_t muted);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_listen(cimsue_engine_t* e, int32_t call_id, int32_t listen);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_rx_level(cimsue_engine_t* e, int32_t call_id, float level);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_dtmf(cimsue_engine_t* e, int32_t call_id, const char* digits);
CIMSUE_API void CIMSUE_CALL cimsue_engine_call_info(const cimsue_engine_t* e, int32_t call_id, cimsue_call_info_t* out);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_calls(const cimsue_engine_t* e, const int32_t** out);
CIMSUE_API void CIMSUE_CALL cimsue_engine_stream_stats(const cimsue_engine_t* e, int32_t call_id,
                                                       cimsue_stream_stats_t* out);
/* 호 품질(Engine::callQuality) — 손실·폐기·지터·RTD·E-model MOS. 없는 호는 valid=0 */
CIMSUE_API void CIMSUE_CALL cimsue_engine_call_quality(const cimsue_engine_t* e, int32_t call_id,
                                                       cimsue_call_quality_t* out);
/* SIP TLS 서버 인증서 만료 관측(Engine::tlsPeerExpiry) — 관제조작반 경고 입력(sip_tls_signaling.md §8.6.2) */
CIMSUE_API void CIMSUE_CALL cimsue_engine_tls_peer_expiry(const cimsue_engine_t* e, cimsue_tls_peer_expiry_t* out);

/* MCPTT 그룹콜·사설콜 (TS 24.379) — opts=NULL 이면 기본값 */
CIMSUE_API void CIMSUE_CALL cimsue_group_call_options_default(cimsue_group_call_options_t* opts);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_join_group_call(cimsue_engine_t* e, int32_t account_id,
                                                             const char* group_id,
                                                             const cimsue_group_call_options_t* opts);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_start_private_call(cimsue_engine_t* e, int32_t account_id,
                                                                const char* peer,
                                                                const cimsue_group_call_options_t* opts);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_leave_group_call(cimsue_engine_t* e, int32_t call_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_floor_request(cimsue_engine_t* e, int32_t call_id,
                                                                   int32_t priority);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_floor_release(cimsue_engine_t* e, int32_t call_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_floor_queue_cancel(cimsue_engine_t* e, int32_t call_id);
CIMSUE_API void CIMSUE_CALL cimsue_engine_floor_info(const cimsue_engine_t* e, int32_t call_id,
                                                     cimsue_floor_info_t* out);
/** 진행 중 그룹콜의 조건 상향·하향(Engine::setCallCondition, TS 24.379 §10.1.1.2.1.3~5) — 결과는 on_mcptt_condition
 *  (LOCAL → CONFIRMED/DENIED). 바뀐 것이 없으면 보내지 않는다. 사설콜·응답 대기 중·성립 전 호는 실패. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_call_condition(cimsue_engine_t* e, int32_t call_id,
                                                                        int32_t emergency, int32_t imminent_peril);
/** 긴급 경보 발신·취소(Engine::sendEmergencyAlert, TS 24.379 §12.1.1.1·§12.1.1.2). originated_by(NULL 가능) = 다른 사용자의
 *  경보를 취소할 때 그 사용자 MCPTT ID, cancel_group_emergency = 그룹의 진행 중 긴급도 해제. 반환 token(on_request_result
 *  MESSAGE 상관), 실패 -1. */
CIMSUE_API int64_t CIMSUE_CALL cimsue_engine_send_emergency_alert(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                                  int32_t activate, const char* originated_by,
                                                                  int32_t cancel_group_emergency);

CIMSUE_API int64_t CIMSUE_CALL cimsue_engine_affiliate(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                       int32_t on);
/** 서비스를 고르는 affiliation(Engine::affiliate(…, service)) — MCPTT = cimsue_engine_affiliate, MCVideo = 관심 그룹 전부를 한 PUBLISH 로
 *  (TS 24.281 §8.2.1.2). 반환 token(on_request_result 상관), 실패 -1. */
CIMSUE_API int64_t CIMSUE_CALL cimsue_engine_affiliate_service(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                               int32_t on, cimsue_mc_service_t service);

/* MCVideo 그룹 호 (TS 24.281 호 · TS 24.581 전송 제어) — opts=NULL 이면 기본값. 나가기 = cimsue_engine_hangup */
CIMSUE_API void CIMSUE_CALL cimsue_video_group_call_options_default(cimsue_video_group_call_options_t* opts);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_join_video_group_call(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                                   const cimsue_video_group_call_options_t* opts);
/** [영상 보내기] — Transmission Request(§6.2.4.3.2). priority<0 = 미기재. 결과는 on_transmission. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_request_transmission(cimsue_engine_t* e, int32_t call_id, int32_t priority);
/** [보내기 끝] — Transmission End Request(§6.2.4.5.3). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_release_transmission(cimsue_engine_t* e, int32_t call_id);
/** [받기] — Receive Media Request(§6.2.5.3.3). transmitter_id = on_reception(NOTIFIED) 의 transmitter.user_id. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_accept_reception(cimsue_engine_t* e, int32_t call_id, const char* transmitter_id,
                                                                      int32_t priority);
/** [그만 보기] — Media Reception End Request(§6.2.5.5). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_end_reception(cimsue_engine_t* e, int32_t call_id, const char* transmitter_id);
CIMSUE_API void CIMSUE_CALL cimsue_engine_transmission_info(const cimsue_engine_t* e, int32_t call_id, cimsue_transmission_info_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_subscribe_conference(cimsue_engine_t* e, int32_t account_id,
                                                                          const char* group_id, int32_t on);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_subscribe_xcap_diff(cimsue_engine_t* e, int32_t account_id,
                                                                         const char* psi_uri, int32_t on);
/** 임의 SIP 요청. headers 는 (ptr, count) — 없으면 NULL/0. 반환 token(on_request_result 상관), 실패 -1. */
CIMSUE_API int64_t CIMSUE_CALL cimsue_engine_send_request(cimsue_engine_t* e, int32_t account_id, const char* method,
                                                          const char* target_uri, const char* content_type,
                                                          const char* body, const cimsue_header_t* headers,
                                                          int32_t header_count);

/* 관제 (dispatch_center.md §5, volte_supplementary_services.md §5·§6) */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_dialog_watch(cimsue_engine_t* e, int32_t account_id,
                                                                  const char* target_aor, int32_t on);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_join(cimsue_engine_t* e, int32_t account_id, const char* target_uri,
                                                  const cimsue_dialog_info_t* dlg);
/** 그룹 픽업 = feature_code 만, 지정 픽업 = feature_code + number(NULL 이면 그룹). */
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_pickup(cimsue_engine_t* e, int32_t account_id, const char* feature_code,
                                                    const char* number);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_transfer(cimsue_engine_t* e, int32_t call_id, const char* target);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_transfer_attended(cimsue_engine_t* e, int32_t call_id,
                                                                       int32_t consult_call_id);

/* MCData SDS (TS 24.282 §9.2.2 C-plane) */
/** 그룹 SDS 발신. msg_id(NULL·빈 값 = 새로 만든다) = 재전송이면 처음의 message ID(UUID hex32 — 받는 쪽이 같은 메시지로 대조,
 *  hex32 가 아니면 실패). 성공 시 msg_id_out 에 쓴 ID 를 NUL 종료로 기록한다(33바이트면 충분).
 *  token_out(NULL 가능) = 요청 token — 최종 응답 on_request_result 상관용. 본문이 계정의 max_sds_cplane_bytes 를 넘으면 media plane
 *  (MSRP)으로 가고 최종 결과는 method "MSRP" 로 온다(token 상관은 같다). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_group_sds(cimsue_engine_t* e, int32_t account_id,
                                                                    const char* group_id, const char* text,
                                                                    int32_t request_delivery, const char* msg_id,
                                                                    char* msg_id_out, int32_t msg_id_cap, int64_t* token_out);
/** 1:1 SDS 발신(request-type one-to-one-sds). peer = 상대 bare 번호. 나머지는 그룹과 같다(1:1 은 늘 시그널링 평면). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_sds(cimsue_engine_t* e, int32_t account_id,
                                                              const char* peer, const char* text,
                                                              int32_t request_delivery, const char* msg_id,
                                                              char* msg_id_out, int32_t msg_id_cap, int64_t* token_out);
/** SDS disposition 통지(Engine::sendSdsNotification, TS 24.282 §12.2.1.1) — peer = 받은 SDS 의 from_uri, group_id = 받은 SDS 의
 *  group_uri(NULL·빈 값 = 1:1). 계정 mcdata_server_uri 가 있으면 규격형(PSI·resource-lists·mcdata-calling-group-id).
 *  token_out(NULL 가능) = 요청 token. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_sds_notification(cimsue_engine_t* e, int32_t account_id,
                                                                           const char* peer, const char* conv_id,
                                                                           const char* msg_id, const char* group_id,
                                                                           int32_t notif_type, int64_t* token_out);

/* MCData FD (TS 24.282 §10.2 — 파일은 먼저 cimsue_csc_upload_fd 로 올린다) */
/** 그룹 FD 알림 발신(request-type group-fd). msg_id_out·token_out 규약은 send_group_sds 와 같다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_group_fd(cimsue_engine_t* e, int32_t account_id,
                                                                   const char* group_id, const cimsue_fd_file_t* file,
                                                                   char* msg_id_out, int32_t msg_id_cap, int64_t* token_out);
/** 1:1 FD 알림 발신(request-type one-to-one-fd). peer = 상대 bare 번호. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_fd(cimsue_engine_t* e, int32_t account_id,
                                                             const char* peer, const cimsue_fd_file_t* file,
                                                             char* msg_id_out, int32_t msg_id_cap, int64_t* token_out);

/* 장치 */
/** 반환 개수, *out 은 스냅샷 배열(다음 조회까지 유효). */
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_audio_devices(const cimsue_engine_t* e,
                                                           const cimsue_audio_device_info_t** out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_refresh_audio_devices(cimsue_engine_t* e);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_audio_devices(cimsue_engine_t* e, int32_t capture_dev,
                                                                       int32_t playback_dev);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_add_playback_route(cimsue_engine_t* e, int32_t playback_dev);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_remove_playback_route(cimsue_engine_t* e, int32_t route_id);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_call_route(cimsue_engine_t* e, int32_t call_id,
                                                                    int32_t route_id);
/** 캡처 게이트(Engine::setCaptureEnabled) — 0 = 캡처 스트림을 열지 않는다(재생만). 장치 선택을 넘어 유지. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_capture_enabled(cimsue_engine_t* e, int32_t on);
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_capture_enabled(const cimsue_engine_t* e);
/** 장치 단 음량(Engine::setDeviceAudioLevels) — speaker = 스피커 배율(1 = 원음), mic_target_dbov = 마이크 AGC 목표(-40..-10,
 *  기본 CIMSUE_MIC_AGC_TARGET_DBOV). 코어가 기억해 게이트 전환·재오픈·호 결선 뒤 다시 건다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_device_audio_levels(cimsue_engine_t* e, float speaker,
                                                                             double mic_target_dbov);
/** 오디오 라우트(Engine::setAudioRoute) — 모바일 라우트(pjmedia OUTPUT/INPUT_ROUTE). 라우트가 없는 장치(데스크톱)는 무시될 수 있다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_audio_route(cimsue_engine_t* e, cimsue_audio_route_t output,
                                                                     cimsue_audio_route_t input);
/** 사운드 장치 재오픈(Engine::reopenAudioDevice) — 열려 있으면 닫고 곧바로 다시 연다. 닫혀 있으면 아무것도 하지 않는다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_reopen_audio_device(cimsue_engine_t* e);
/** 수신 영상 렌더 대상(Engine::setVideoWindow) — 플랫폼 창 핸들(NULL = 해제). 영상 없는 빌드면 실패. 프레임 렌더 빌드(Windows)도
 *  실패한다 — 프레임이 on_video_frame 으로 온다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_window(cimsue_engine_t* e, void* native_window);
/** 셀프뷰 프레임(Engine::setVideoPreview — 프레임 렌더 빌드만) — on 이면 내 영상을 보내는 동안 카메라 프레임을 on_video_frame
 *  (call_id -1)으로도 넘긴다. 카메라는 송출이 연다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_preview(cimsue_engine_t* e, int32_t on);
/** 캡처 카메라 선택(Engine::setVideoCaptureDevice) — cimsue_engine_video_devices 의 캡처 장치 id, -1 = 기본. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_capture_device(cimsue_engine_t* e, int32_t device_id);
/** 내 영상 송출 허용(Engine::setVideoSend — CallInfo.videoSend). MCPTT 반이중은 허용이면서 발언권을 가진 동안만 보낸다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_set_video_send(cimsue_engine_t* e, int32_t call_id, int32_t on);
/** 캡처 카메라 전환(Engine::switchCamera). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_switch_camera(cimsue_engine_t* e, int32_t call_id);
/** 영상 장치 목록. 반환 개수, *out 은 스냅샷 배열(다음 조회까지 유효). */
CIMSUE_API int32_t CIMSUE_CALL cimsue_engine_video_devices(const cimsue_engine_t* e,
                                                           const cimsue_video_device_info_t** out);

/** 라이브러리·엔진 버전(정적 문자열). */
CIMSUE_API const char* CIMSUE_CALL cimsue_version(void);
/** 마지막 실패의 사유(스레드별). 실패를 돌려받은 직후에만 의미가 있다. */
CIMSUE_API const char* CIMSUE_CALL cimsue_last_error(void);

/* toString (types.h) — 정적 문자열 */
CIMSUE_API const char* CIMSUE_CALL cimsue_reg_state_str(cimsue_reg_state_t s);
CIMSUE_API const char* CIMSUE_CALL cimsue_call_state_str(cimsue_call_state_t s);
CIMSUE_API const char* CIMSUE_CALL cimsue_transport_str(cimsue_transport_t t);
CIMSUE_API const char* CIMSUE_CALL cimsue_floor_state_str(cimsue_floor_state_t s);
CIMSUE_API const char* CIMSUE_CALL cimsue_floor_kind_str(cimsue_floor_kind_t k);
CIMSUE_API const char* CIMSUE_CALL cimsue_condition_cause_str(cimsue_condition_cause_t c);
CIMSUE_API const char* CIMSUE_CALL cimsue_mc_service_str(cimsue_mc_service_t s);
CIMSUE_API const char* CIMSUE_CALL cimsue_transmission_state_str(cimsue_transmission_state_t s);
CIMSUE_API const char* CIMSUE_CALL cimsue_reception_state_str(cimsue_reception_state_t s);
CIMSUE_API const char* CIMSUE_CALL cimsue_transmission_kind_str(cimsue_transmission_kind_t k);
CIMSUE_API const char* CIMSUE_CALL cimsue_reception_kind_str(cimsue_reception_kind_t k);

/* ── 문자열 산출 헬퍼 (C++ 인라인 멤버·types.h 자유 함수 1:1) ──
 * 공통 규약: out 에 최대 cap 바이트(NUL 포함)를 NUL 종료로 기록하고, NUL 을 제외한 실제 길이를 반환한다.
 * 반환값이 cap 이상이면 잘린 것이다. out=NULL·cap=0 이면 필요한 길이만 계산한다. */
CIMSUE_API int32_t CIMSUE_CALL cimsue_account_config_aor(const cimsue_account_config_t* cfg, char* out, int32_t cap);
CIMSUE_API int32_t CIMSUE_CALL cimsue_account_config_mcptt_id(const cimsue_account_config_t* cfg, char* out,
                                                              int32_t cap);
/** Digest username = 전체 IMPI. msisdn 폴백 없음(서버는 불일치 시 즉시 403). */
CIMSUE_API int32_t CIMSUE_CALL cimsue_account_config_digest_username(const cimsue_account_config_t* cfg, char* out,
                                                                     int32_t cap);
CIMSUE_API int32_t CIMSUE_CALL cimsue_account_config_is_complete(const cimsue_account_config_t* cfg);
/** Join 헤더 값 — <call-id>;to-tag=<remote-tag>;from-tag=<local-tag>. */
CIMSUE_API int32_t CIMSUE_CALL cimsue_dialog_info_join_header(const cimsue_dialog_info_t* d, char* out, int32_t cap);
/** REGISTER User-Agent 규약 `<제품>/<앱 버전> (<OS>; <모델>)` — userAgentOf(types.h, comment 정리 포함). NULL 인자 = 빈 값. */
CIMSUE_API int32_t CIMSUE_CALL cimsue_user_agent_of(const char* product, const char* version, const char* os,
                                                    const char* model, char* out, int32_t cap);
/** IMEI(15자리 CD 형식·14자리·끝 0 전송 형식) → RFC 7254 instance URN(spare 0) — imeiUrn(types.h). 자릿수·검사 숫자가 틀리면 빈 문자열(반환 0). */
CIMSUE_API int32_t CIMSUE_CALL cimsue_imei_urn(const char* imei, char* out, int32_t cap);

/* ── CSC 설정 평면 (csc.h) ──
 * 산출 구조체(토큰·프로파일·그룹·XCAP 문서)의 문자열·배열은 같은 핸들에 다음 호출을 할 때까지 유효하다. */

typedef struct {
    const char* host;
    int32_t     port;
    const char* client_id;
    const char* redirect_uri;
    const char* scope;
    const char* ca_pem;                 /* 신뢰 앵커(NULL = 시스템 기본) */
    int32_t     verify_server;
} cimsue_csc_endpoint_t;

typedef struct {
    const char* access_token;
    const char* token_type;
    const char* refresh_token;
    const char* id_token;
    const char* scope;
    int32_t     expires_in_sec;
} cimsue_token_set_t;

typedef struct {
    cimsue_transport_t transport;
    int32_t            port;
} cimsue_service_endpoint_t;

typedef struct {
    const char*                      kind;           /* volte | ptt */
    const char*                      sip_host;
    int32_t                          sip_port;
    cimsue_transport_t               transport;
    const cimsue_service_endpoint_t* transports;
    int32_t                          transport_count;
    int32_t                          enforced;
    cimsue_media_security_t          media_security;
    const char*                      domain;
    const char*                      msisdn;
    const char*                      imsi;
    const char*                      auth_id;
    const char*                      sip_ha1;
    const char*                      mcptt_id;
    cimsue_auth_scheme_t             auth_scheme;
    const char*                      aka_k;
    const char*                      aka_opc;
    const char*                      aka_amf;
    const char* const*               sec_mechanisms;
    int32_t                          sec_mechanism_count;
    int32_t                          max_payload_sds_cplane_bytes;
    /* 끝에 덧붙였다 */
    int32_t                          udp_no_tcp_switch;   /* sip.udpNoTcpSwitch — 엔진 전역(engine_config.udp_no_tcp_switch)이라 앱이 고른다 */
    int32_t                          sms_gateway;         /* capabilities.smsGateway — 외부망 SMS/LMS 게이트웨이 연결 */
} cimsue_service_profile_t;

/** 관제 그룹원(dispatch members[]) — dialog 구독·그룹원 띠 대상. */
typedef struct {
    const char* user_id;
    const char* name;
    const char* volte_aor;
    const char* ptt_id;
    const char* extension;
    const char* group_id;               /* 그 가입자의 관제 그룹(무소속 "") — 그룹원 띠 = group_id == dispatch.group_id */
} cimsue_dispatch_member_t;

/** 청취 대상 PTT 그룹(dispatch pttTargets[]). */
typedef struct {
    const char* id;
    const char* uri;
    const char* name;
} cimsue_dispatch_target_t;

/** 관제 데스크(dispatch_center.md §8.4) — 없으면 present=0. members/ptt_targets 는 서버 미제공 시 빈 배열. */
typedef struct {
    int32_t                         present;
    const char*                     group_id;
    const char*                     group_name;
    const char*                     pilot_id;
    const char*                     monitor_scope;          /* none|own|listed|all */
    const char*                     ptt_listen;
    const char*                     listen_visibility;
    const char*                     directory_admin;        /* none|own|all — 관제 앱 관리 범위 */
    const char*                     org_code;               /* 관제 그룹 소속 조직 코드("" = 없음) */
    const cimsue_dispatch_member_t* members;
    int32_t                         member_count;
    const cimsue_dispatch_target_t* ptt_targets;
    int32_t                         ptt_target_count;
} cimsue_dispatch_profile_t;

typedef struct {
    const char*                     display_name;
    const char*                     login_id;
    const char*                     country_code;
    const char*                     csc_host;
    int32_t                         csc_port;
    const cimsue_service_profile_t* services;
    int32_t                         service_count;
    cimsue_dispatch_profile_t       dispatch;
    int32_t                         allow_group_creation;   /* GMS 그룹 생성 자격 */
} cimsue_profile_t;

typedef struct {
    const char* uri;
    const char* display_name;
    const char* etag;
    int32_t     member_count;
    int32_t     is_owner;               /* 토큰 주체가 authorized user(편집·삭제 가능) */
} cimsue_group_summary_t;

typedef struct {
    const char* body;
    const char* etag;
    int32_t     not_modified;
} cimsue_xcap_doc_t;

/** 범용 HTTP 요청 산출(csc.h HttpResult). body 는 바이트(이진 가능) + 길이 — NUL 종료를 가정하지 않는다. */
typedef struct {
    int32_t        status;          /* HTTP 상태(0 = 전송 실패) */
    const char*    content_type;
    const char*    etag;
    const uint8_t* body;
    int32_t        body_len;
} cimsue_http_result_t;

/** MCData FD 업로드 결과(csc.h FdUpload) — url 을 cimsue_fd_file_t.url 로 넘긴다. */
typedef struct {
    const char* id;
    const char* url;
    const char* name;
    int64_t     size;
} cimsue_fd_upload_t;

/** 그룹 문서 멤버 — role = chair | participant. */
typedef struct {
    const char* uri;
    const char* display_name;
    const char* role;
    int32_t     priority;
    int32_t     required;      /* 필수 멤버 <on-network-required>(TS 24.481 §7.2.4.2) — 끝에 덧붙였다(64비트 크기 불변) */
    const char* title;         /* 직함 <cims:user-title> — 산출 전용(PUT 에 싣지 않는다). 끝에 덧붙였다 */
    const char* mcvideo_id;    /* <mcvideo-mcvideo-id uri>(TS 24.481 §7.2.2 MCVideo entry) — NULL·빈 값 = uri 와 같다. 끝에 덧붙였다 */
} cimsue_group_member_t;

/** 그룹 문서의 MCVideo 몫(csc.h McVideoGroupAttrs — TS 24.481 §7.2.2·§7.2.8). present = 0 이면 나머지를 보지 않고 PUT 에 MCVideo 를 싣지 않는다
 *  (서버는 MCVideo `<service>` 가 없는 PUT 으로 그 그룹의 MCVideo 설정을 바꾸지 않는다 — 전환기). present = 1 로 쓸 때는
 *  cimsue_mcvideo_group_attrs_default() 로 채운 뒤 바꾼다 — 정수 -1 = 미기재, allow_* 삼중값 -1 = 미기재 / 0 / 1, 보호 둘은 기본 1(요소가 없으면 true). */
typedef struct {
    int32_t            present;
    int32_t            invite_members;                 /* 1 = prearranged, 0 = chat(mcvideo.md §7 D5) */
    int32_t            max_duration_sec;
    int32_t            protect_media;
    int32_t            protect_transmission_control;
    const char* const* audio_encodings;                /* mcvideo-preferred-audio-encodings (ptr, count) */
    int32_t            audio_encoding_count;
    const char* const* video_encodings;
    int32_t            video_encoding_count;
    const char*        video_resolutions;
    const char*        video_frame_rate;
    int32_t            urgent_real_time_video_mode;
    int32_t            non_urgent_real_time_video_mode;
    int32_t            non_real_time_video_mode;
    const char*        active_real_time_video_mode;
    int32_t            max_transmitters;               /* 동시 송출 상한 */
    int32_t            min_number_to_start;
    int32_t            group_priority;
    int32_t            reception_hang_timer_sec;       /* on-network-reception-hang-timer (T5) */
    int32_t            allow_conference_state;
    int32_t            allow_emergency_call;
    int32_t            allow_emergency_alert;
    int32_t            allow_imminent_peril_call;
} cimsue_mcvideo_group_attrs_t;

/** GMS 그룹 문서(csc.h GroupDoc) — GET 산출·PUT 입력 공용. 입력 시 문자열 NULL 은 빈 값, members NULL 은 멤버 없음. */
typedef struct {
    const char*                  uri;
    const char*                  display_name;
    const char*                  etag;                  /* 산출 전용(입력은 if_match 인자) */
    const cimsue_group_member_t* members;
    int32_t                      member_count;
    const char*                  session_type;          /* 그룹 종류 prearranged | chat (NULL = prearranged) */
    int32_t                      encryption;
    int32_t                      emergency_call;
    int32_t                      emergency_alert;
    int32_t                      allow_sds;
    int32_t                      allow_fd;
    int32_t                      require_affiliation;
    int32_t                      priority;
    int32_t                      max_participants;      /* 0 = 미기재 */
    const char*                  org_code;
    const char*                  authorized_user;       /* 산출 전용 */
    /* 그룹 호 타이머·참가자 정보·MCData 크기 한도(TS 24.481). has_* = 0 이면 **미기재** — PUT 에 싣지 않아 서버가
     * 기존값을 유지한다. 값과 존재를 나누는 이유: 0 이 뜻을 갖는 필드가 있고(hang 0 = 미사용, 크기 0 = 무제한),
     * 0 으로 채워진 구조체(.NET 기본값)가 «미기재» 로 읽혀야 폼에서 이 칸을 다루지 않는 앱이 서버 값을 덮지 않는다.
     * 구조체 끝에 덧붙였다 — 앞 필드의 오프셋은 그대로다. */
    int32_t                      has_hang_timer;
    int32_t                      hang_timer_sec;        /* on-network-hang-timer (T4) — 0 = 미사용 */
    int32_t                      has_max_duration;
    int32_t                      max_duration_sec;      /* on-network-maximum-duration (TNG3) — 0 = 무제한 */
    int32_t                      has_conference_state;
    int32_t                      allow_conference_state;/* on-network-allow-conference-state */
    int32_t                      has_max_sds_size;
    int32_t                      max_sds_size;          /* mcdata-on-network-max-data-size-for-SDS — 0 = 무제한 */
    int32_t                      has_max_auto_recv;
    int32_t                      max_auto_recv;         /* mcdata-on-network-max-data-size-auto-recv — 0 = 무제한 */
    /* 확인 통화 설정(TS 24.481 §7.2.2 s)t)u)) — 같은 미기재 규약. ack_action NULL·빈 값 = 미기재. 끝에 덧붙였다. */
    int32_t                      has_min_number_to_start;
    int32_t                      min_number_to_start;   /* on-network-minimum-number-to-start — 0 = 기다리지 않음 */
    int32_t                      has_ack_timeout;
    int32_t                      ack_timeout_sec;       /* on-network-timeout-for-acknowledgement-of-required-members (TNG1) */
    const char*                  ack_action;            /* proceed | abandon */
    cimsue_mcvideo_group_attrs_t mcvideo;               /* MCVideo 몫 — present = 0 이면 PUT 에 싣지 않는다. 끝에 덧붙였다 */
} cimsue_group_doc_t;

/** CMS 대상 항목(csc.h CmsEntry, TS 24.484 §8.3.2.7 EntryType) — mode = entry-info 속성. */
typedef struct {
    const char* uri;
    const char* mode;
} cimsue_cms_entry_t;

/** MCPTT user profile(csc.h UserProfileDoc, TS 24.484 §8.3.2) — 인가 allow-* 는 요소가 없으면 허용(1). */
typedef struct {
    const char*        etag;
    int32_t            not_modified;        /* fetch 가 304 를 받았다 — 나머지는 비어 있다(호출자 사본 유지) */
    const char*        user_uri;
    cimsue_cms_entry_t emergency_group;
    cimsue_cms_entry_t imminent_peril_group;
    cimsue_cms_entry_t emergency_alert_group;
    cimsue_cms_entry_t emergency_private_recipient;
    const char* const* groups;              /* 제휴 가능 그룹 URI (ptr, count) */
    int32_t            group_count;
    const char* const* implicit_affiliations;
    int32_t            implicit_affiliation_count;
    int32_t            max_affiliations_n2; /* -1 = 미기재 */
    int32_t            allow_private_call;
    int32_t            allow_emergency_group_call;
    int32_t            allow_imminent_peril_call;
    int32_t            allow_activate_emergency_alert;
    int32_t            allow_cancel_emergency_alert;
    int32_t            allow_emergency_private_call;
    int32_t            allow_adhoc_group_call;
    int32_t            allow_cancel_group_emergency;  /* allow-cancel-group-emergency (TS 24.484 §8.3.2.1 11)xiv)) */
    int32_t            allow_cancel_imminent_peril;   /* allow-cancel-imminent-peril (11)xvii)) */
} cimsue_user_profile_doc_t;

/** MCPTT service configuration(csc.h ServiceConfigDoc, TS 24.484 §8.4) — 인가 요소 없음. 요소가 없으면 빈 값/-1. */
typedef struct {
    const char* etag;
    int32_t     not_modified;
    const char* domain;
    int32_t     num_levels_group_hierarchy;
    int32_t     num_levels_user_hierarchy;
    const char* rp_emergency;               /* Resource-Priority r-value — 빈 값 = 미기재(계정 기본값 유지) */
    const char* rp_imminent_peril;
    const char* rp_normal;
} cimsue_service_config_doc_t;

/** MCVideo user profile(csc.h McVideoUserProfileDoc, TS 24.484 §9.3) — 문서가 있으면 MCVideo 이용 자격이 있다(fetch 404 = 자격 없음).
 *  인가 allow-* 는 요소가 없으면 허용(1). */
typedef struct {
    const char*        etag;
    int32_t            not_modified;
    const char*        user_uri;
    const char*        mcvideo_id;
    const char* const* groups;                          /* MCVideo 로 affiliate 할 수 있는 그룹 (ptr, count) */
    int32_t            group_count;
    const char* const* implicit_affiliations;
    int32_t            implicit_affiliation_count;
    int32_t            max_affiliations_n2;             /* -1 = 미기재 */
    int32_t            max_simultaneous_video_streams;  /* 동시 수신 스트림 상한(서버 C9) */
    int32_t            max_simultaneous_calls_n6;
    cimsue_cms_entry_t emergency_group;
    cimsue_cms_entry_t imminent_peril_group;
    cimsue_cms_entry_t emergency_alert_group;
    int32_t            allow_private_call;
    int32_t            allow_emergency_group_call;
    int32_t            allow_emergency_private_call;
    int32_t            allow_imminent_peril_call;
    int32_t            allow_activate_emergency_alert;
    int32_t            allow_revoke_transmit;
    int32_t            allow_remote_ambient_viewing;
    int32_t            allow_local_ambient_viewing;
    int32_t            allow_adhoc_group_call;
} cimsue_mcvideo_user_profile_doc_t;

/** MCVideo service configuration(csc.h McVideoServiceConfigDoc, TS 24.484 §9.4) — 참여자 전송 제어 타이머 T100~T104(초, -1 = 미기재 →
 *  K5 기본값)·RP·신호 보호(요소가 없으면 켜짐). */
typedef struct {
    const char* etag;
    int32_t     not_modified;
    const char* domain;
    const char* rp_emergency;
    const char* rp_imminent_peril;
    const char* rp_normal;
    int32_t     confidentiality_protection;
    int32_t     integrity_protection;
    int32_t     t100_sec, t101_sec, t102_sec, t103_sec, t104_sec;
} cimsue_mcvideo_service_config_doc_t;

/** MCS UE initial configuration(csc.h UeInitConfigDoc, TS 24.484 §7.2) — 참여 기능 PSI. 광고하지 않은 서비스는 빈 값. */
typedef struct {
    const char* etag;
    int32_t     not_modified;
    const char* domain;
    const char* mcptt_server_uri;           /* MCPTT-Service-Details/Server-URI → 계정 mcptt_server_uri */
    const char* mcdata_server_uri;          /* MCData-Service-Details/Server-URI → 계정 mcdata_server_uri */
    const char* mcvideo_server_uri;         /* MCVideo-Service-Details/Server-URI → 계정 mcvideo_server_uri(끝에 덧붙였다) */
} cimsue_ue_init_config_doc_t;

/** 정책 게이트 스냅샷(csc.h Capabilities) — 받지 못한 문서는 허용. UX 선차단용, 최종 판정은 서버. */
typedef struct {
    int32_t user_profile_known;
    int32_t service_config_known;
    int32_t private_call;
    int32_t emergency_group_call;
    int32_t imminent_peril_call;
    int32_t emergency_private_call;
    int32_t emergency_alert;
    int32_t cancel_emergency_alert;
    int32_t adhoc_group_call;
    int32_t max_affiliations_n2;            /* 0 = 미지정 */
    int32_t cancel_group_emergency;         /* up.allow-cancel-group-emergency — 앱은 «내가 올린 조건» 과 OR (§6.3.3.1.13.4) */
    int32_t cancel_imminent_peril;          /* up.allow-cancel-imminent-peril */
} cimsue_capabilities_t;

CIMSUE_API void CIMSUE_CALL cimsue_csc_endpoint_default(cimsue_csc_endpoint_t* ep);
CIMSUE_API cimsue_csc_t* CIMSUE_CALL cimsue_csc_create(const cimsue_csc_endpoint_t* ep);
CIMSUE_API void CIMSUE_CALL cimsue_csc_destroy(cimsue_csc_t* c);

/** IdMS PKCE(S256) 로그인 → 토큰. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_login(cimsue_csc_t* c, const char* user_name, const char* password,
                                                        cimsue_token_set_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_refresh(cimsue_csc_t* c, const char* refresh_token,
                                                          cimsue_token_set_t* out);
/** GET /provisioning/me */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_profile(cimsue_csc_t* c, const char* access_token,
                                                                cimsue_profile_t* out);
/** GMS 그룹 목록. 반환 개수(실패 -1), *out 은 핸들 스냅샷 배열. */
CIMSUE_API int32_t CIMSUE_CALL cimsue_csc_list_groups(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                      const cimsue_group_summary_t** out);
/** XCAP GET — if_none_match(NULL 가능) 로 304 캐시. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_xcap_get(cimsue_csc_t* c, const char* access_token, const char* path,
                                                           const char* accept, const char* if_none_match,
                                                           cimsue_xcap_doc_t* out);
/** 범용 요청(Bearer) — 코어가 모델링하지 않은 CSC 엔드포인트(관제 관리 API·녹취·이력 창 조회). body/body_len 은 본문(NULL/0 = 없음),
 *  content_type·accept·if_match·if_none_match 는 NULL 가능. 2xx·304 = 0(out->status 로 구분), 그 밖의 HTTP 상태 = 그 값(out 은
 *  채워진다 — 오류 JSON 본문), 전송 실패 = -1. 산출은 핸들 스냅샷(다음 호출 전까지 유효). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_request(cimsue_csc_t* c, const char* access_token, const char* method,
                                                          const char* path, const char* content_type, const uint8_t* body,
                                                          int32_t body_len, const char* accept, const char* if_match,
                                                          const char* if_none_match, cimsue_http_result_t* out);
/** MCData FD 업로드(octet-stream). group_id(NULL·빈 값 = 1:1)면 서버가 allow_fd·멤버십으로 게이트. 반환 = cimsue_csc_request 규약
 *  (0 / HTTP 상태 — 403 게이트·404 그룹·413 상한 / -1 전송 실패). *out 은 핸들 스냅샷. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_upload_fd(cimsue_csc_t* c, const char* access_token, const uint8_t* data,
                                                            int32_t data_len, const char* name, const char* mime,
                                                            const char* group_id, cimsue_fd_upload_t* out);
/** MCData FD 다운로드 — url = 받은 FILEURL. 경로(/mcdata/fd/{id})만 취해 이 CSC 에 요청(Bearer 를 다른 호스트로 보내지 않음),
 *  FD 경로가 아니면 -2. 산출 = cimsue_csc_request 와 같은 핸들 스냅샷(body = 파일 바이트). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_download_fd(cimsue_csc_t* c, const char* access_token, const char* url,
                                                              cimsue_http_result_t* out);
/* HTTPS 서버 인증서 만료 관측(CscClient::tlsPeerExpiry) — 문자열은 그 핸들의 다음 호출까지 유효 */
CIMSUE_API void CIMSUE_CALL cimsue_csc_tls_peer_expiry(cimsue_csc_t* c, cimsue_tls_peer_expiry_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_get_user_profile(cimsue_csc_t* c, const char* access_token,
                                                                   const char* user_uri, const char* etag,
                                                                   cimsue_xcap_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_get_service_config(cimsue_csc_t* c, const char* access_token,
                                                                     const char* user_uri, const char* etag,
                                                                     cimsue_xcap_doc_t* out);
/** user-profile GET + 해석(CscClient::fetchUserProfile) — etag(NULL 가능) = If-None-Match. 304 면 out->not_modified=1.
 *  해석 실패는 -2. *out 은 핸들 스냅샷. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_user_profile(cimsue_csc_t* c, const char* access_token,
                                                                     const char* user_uri, const char* etag,
                                                                     cimsue_user_profile_doc_t* out);
/** service-config GET + 해석(CscClient::fetchServiceConfig) — fetch_user_profile 과 같은 규약. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_service_config(cimsue_csc_t* c, const char* access_token,
                                                                       const char* user_uri, const char* etag,
                                                                       cimsue_service_config_doc_t* out);
/** UE initial configuration GET + 해석(CscClient::fetchUeInitConfig) — mcs_ue_id = 단말 instance ID(urn:uuid:…), 토큰 없음(로그인 전).
 *  etag·304·해석 실패 규약은 fetch_user_profile 과 같다. *out 은 핸들 스냅샷. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_ue_init_config(cimsue_csc_t* c, const char* mcs_ue_id, const char* etag,
                                                                       cimsue_ue_init_config_doc_t* out);
/** XML → 문서(시험·캐시용). 산출은 스레드별 스냅샷. 루트가 다르면 -1(사유 last_error). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_user_profile_parse(const char* xml, cimsue_user_profile_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_service_config_parse(const char* xml, cimsue_service_config_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_ue_init_config_parse(const char* xml, cimsue_ue_init_config_doc_t* out);
/** MCVideo 설정 문서(TS 24.484 §9.3·§9.4) — fetch_user_profile 과 같은 규약. user profile 주소 = MCVideo ID(404 = MCVideo 자격 없음),
 *  service configuration 은 시스템 전역 문서라 사용자 인자가 없다. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_mcvideo_user_profile(cimsue_csc_t* c, const char* access_token,
                                                                             const char* mcvideo_id, const char* etag,
                                                                             cimsue_mcvideo_user_profile_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_fetch_mcvideo_service_config(cimsue_csc_t* c, const char* access_token, const char* etag,
                                                                               cimsue_mcvideo_service_config_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_mcvideo_user_profile_parse(const char* xml, cimsue_mcvideo_user_profile_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_mcvideo_service_config_parse(const char* xml, cimsue_mcvideo_service_config_doc_t* out);
/** MCVideo 몫 기본값(csc.h McVideoGroupAttrs) — present = 0, 정수 -1, 보호 둘 1. 켤 때 present = 1 로 바꾼다. */
CIMSUE_API void CIMSUE_CALL cimsue_mcvideo_group_attrs_default(cimsue_mcvideo_group_attrs_t* out);
/** 정책 게이트(Capabilities::of) — NULL = 그 문서를 아직 못 받음. */
CIMSUE_API void CIMSUE_CALL cimsue_capabilities_of(const cimsue_user_profile_doc_t* user_profile,
                                                   const cimsue_service_config_doc_t* service_config,
                                                   cimsue_capabilities_t* out);

/* ── GMS 그룹 관리(TS 24.481 XCAP PUT/DELETE — authorized user = 토큰 주체) ── */
/** 그룹 문서 GET → *out (핸들 스냅샷). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_get_group(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                            const char* group_uri, cimsue_group_doc_t* out);
/** 그룹 생성/수정 — doc 를 PUT. if_match(NULL 가능)로 조건부. 성공 시 *out = 서버 확정 문서(etag 포함). */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_put_group(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                            const cimsue_group_doc_t* doc, const char* if_match,
                                                            cimsue_group_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_delete_group(cimsue_csc_t* c, const char* access_token, const char* user_uri,
                                                               const char* group_uri);
/** 그룹 문서 ↔ XML (시험·캐시용). to_xml 은 문자열 산출 규약, parse 산출은 스레드별 스냅샷. */
CIMSUE_API int32_t CIMSUE_CALL cimsue_group_doc_to_xml(const cimsue_group_doc_t* doc, char* out, int32_t cap);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_group_doc_parse(const char* xml, cimsue_group_doc_t* out);

/** /provisioning/me 응답 JSON → 프로파일 (시험용). 산출은 스레드별 스냅샷. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_parse_profile(const char* json, cimsue_profile_t* out);
CIMSUE_API int32_t CIMSUE_CALL cimsue_csc_enc(const char* s, char* out, int32_t cap);

/** kind 로 서비스 찾기 — 없으면 NULL. profile 이 소유한 배열을 가리킨다(복사 없음). */
CIMSUE_API const cimsue_service_profile_t* CIMSUE_CALL cimsue_profile_service(const cimsue_profile_t* profile,
                                                                              const char* kind);
/** 전화 회선 — 유선 "voip" 우선, 없으면 이동 "volte"(Profile::phoneService). 없으면 NULL. */
CIMSUE_API const cimsue_service_profile_t* CIMSUE_CALL cimsue_profile_phone_service(const cimsue_profile_t* profile);
/** 이 서비스로 등록할 계정 설정. login_pw 는 sip_ha1 이 없을 때의 평문 폴백(NULL 가능).
 *  out 의 문자열은 스레드별 스냅샷 — 같은 스레드의 다음 to_account 호출 전까지 유효하다. */
CIMSUE_API void CIMSUE_CALL cimsue_service_profile_to_account(const cimsue_service_profile_t* sp, const char* login_pw,
                                                              cimsue_account_config_t* out);

/* ── ABI 자기검사 ──
 * 바인딩(P/Invoke 등 손 평탄화 층)이 자기 구조체 정의를 이 DLL 이 실제로 컴파일한 레이아웃과 대조한다 —
 * 헤더와 바인딩의 드리프트를 바인딩 쪽 단위시험이 잡는다(ue_sdk.md §6.4). 구조체를 추가하면 여기에도 등록한다. */
typedef enum {
    CIMSUE_STRUCT_ENGINE_CONFIG = 0, CIMSUE_STRUCT_ACCOUNT_CONFIG, CIMSUE_STRUCT_CALL_OPTIONS,
    CIMSUE_STRUCT_GROUP_CALL_OPTIONS, CIMSUE_STRUCT_HEADER, CIMSUE_STRUCT_REG_INFO, CIMSUE_STRUCT_MCPTT_INFO,
    CIMSUE_STRUCT_MEDIA_SOURCE, CIMSUE_STRUCT_CALL_INFO, CIMSUE_STRUCT_TALKER, CIMSUE_STRUCT_FLOOR_EVENT,
    CIMSUE_STRUCT_FLOOR_INFO, CIMSUE_STRUCT_REQUEST_RESULT, CIMSUE_STRUCT_DIALOG_INFO, CIMSUE_STRUCT_ROSTER_ENTRY,
    CIMSUE_STRUCT_SDS_MESSAGE, CIMSUE_STRUCT_STREAM_STATS, CIMSUE_STRUCT_AUDIO_DEVICE_INFO, CIMSUE_STRUCT_LISTENER,
    CIMSUE_STRUCT_CSC_ENDPOINT, CIMSUE_STRUCT_TOKEN_SET, CIMSUE_STRUCT_SERVICE_ENDPOINT, CIMSUE_STRUCT_SERVICE_PROFILE,
    CIMSUE_STRUCT_DISPATCH_PROFILE, CIMSUE_STRUCT_PROFILE, CIMSUE_STRUCT_GROUP_SUMMARY, CIMSUE_STRUCT_XCAP_DOC,
    CIMSUE_STRUCT_DISPATCH_MEMBER, CIMSUE_STRUCT_DISPATCH_TARGET, CIMSUE_STRUCT_GROUP_MEMBER, CIMSUE_STRUCT_GROUP_DOC,
    CIMSUE_STRUCT_HTTP_RESULT, CIMSUE_STRUCT_TLS_PEER_EXPIRY, CIMSUE_STRUCT_FD_FILE, CIMSUE_STRUCT_FD_UPLOAD,
    CIMSUE_STRUCT_QUALITY_DIRECTION, CIMSUE_STRUCT_CALL_QUALITY,
    CIMSUE_STRUCT_MCPTT_CONDITION, CIMSUE_STRUCT_EMERGENCY_ALERT, CIMSUE_STRUCT_VIDEO_DEVICE_INFO, CIMSUE_STRUCT_CMS_ENTRY,
    CIMSUE_STRUCT_USER_PROFILE_DOC, CIMSUE_STRUCT_SERVICE_CONFIG_DOC, CIMSUE_STRUCT_CAPABILITIES,
    CIMSUE_STRUCT_UE_INIT_CONFIG_DOC,
    CIMSUE_STRUCT_VIDEO_GROUP_CALL_OPTIONS, CIMSUE_STRUCT_VIDEO_TRANSMITTER, CIMSUE_STRUCT_TRANSMISSION_EVENT,
    CIMSUE_STRUCT_RECEPTION_EVENT, CIMSUE_STRUCT_TRANSMISSION_INFO,
    CIMSUE_STRUCT_MCVIDEO_GROUP_ATTRS, CIMSUE_STRUCT_MCVIDEO_USER_PROFILE_DOC, CIMSUE_STRUCT_MCVIDEO_SERVICE_CONFIG_DOC,
    CIMSUE_STRUCT_VIDEO_FRAME,
    CIMSUE_STRUCT_COUNT_
} cimsue_struct_id_t;
/** 구조체의 sizeof(이 DLL 의 컴파일 결과). 모르는 id 는 -1. */
CIMSUE_API int32_t CIMSUE_CALL cimsue_struct_size(cimsue_struct_id_t id);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* CIMSUE_C_H */
