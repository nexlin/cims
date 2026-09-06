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
} cimsue_group_call_options_t;

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
    const char* session_type;           /* prearranged/chat/broadcast/private */
    const char* request_uri;
    const char* calling_user_id;
    const char* calling_group_id;
    int32_t     emergency;
    int32_t     imminent_peril;
    int32_t     private_call;
    int32_t     no_floor_ctrl;
} cimsue_mcptt_info_t;

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
} cimsue_sds_message_t;

typedef struct {
    uint32_t rx_packets, rx_bytes, rx_loss, rx_discard;
    uint32_t tx_packets, tx_bytes;
    int32_t  valid;
} cimsue_stream_stats_t;

typedef struct {
    int32_t     id;
    const char* name;
    const char* driver;
    uint32_t    input_count;
    uint32_t    output_count;
} cimsue_audio_device_info_t;

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

CIMSUE_API int64_t CIMSUE_CALL cimsue_engine_affiliate(cimsue_engine_t* e, int32_t account_id, const char* group_id,
                                                       int32_t on);
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
/** 그룹 SDS 발신. 성공 시 msg_id_out 에 UUID hex32 를 NUL 종료로 기록한다(33바이트면 충분).
 *  token_out(NULL 가능) = 요청 token — 최종 응답 on_request_completed(MESSAGE, token) 상관용. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_group_sds(cimsue_engine_t* e, int32_t account_id,
                                                                    const char* group_id, const char* text,
                                                                    int32_t request_delivery, char* msg_id_out,
                                                                    int32_t msg_id_cap, int64_t* token_out);
/** SDS disposition 통지. token_out(NULL 가능) = 요청 token. */
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_engine_send_sds_notification(cimsue_engine_t* e, int32_t account_id,
                                                                           const char* peer, const char* conv_id,
                                                                           const char* msg_id, int32_t notif_type,
                                                                           int64_t* token_out);

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

/* ── 문자열 산출 헬퍼 (C++ 인라인 멤버 1:1) ──
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
} cimsue_service_profile_t;

/** 관제 그룹원(dispatch members[]) — dialog 구독·그룹원 띠 대상. */
typedef struct {
    const char* user_id;
    const char* name;
    const char* volte_aor;
    const char* ptt_id;
    const char* extension;
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

/** 그룹 문서 멤버 — role = chair | participant. */
typedef struct {
    const char* uri;
    const char* display_name;
    const char* role;
    int32_t     priority;
} cimsue_group_member_t;

/** GMS 그룹 문서(csc.h GroupDoc) — GET 산출·PUT 입력 공용. 입력 시 문자열 NULL 은 빈 값, members NULL 은 멤버 없음. */
typedef struct {
    const char*                  uri;
    const char*                  display_name;
    const char*                  etag;                  /* 산출 전용(입력은 if_match 인자) */
    const cimsue_group_member_t* members;
    int32_t                      member_count;
    const char*                  session_type;          /* prearranged | chat | broadcast (NULL = prearranged) */
    int32_t                      video_enabled;
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
} cimsue_group_doc_t;

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
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_get_user_profile(cimsue_csc_t* c, const char* access_token,
                                                                   const char* user_uri, const char* etag,
                                                                   cimsue_xcap_doc_t* out);
CIMSUE_API cimsue_status_t CIMSUE_CALL cimsue_csc_get_service_config(cimsue_csc_t* c, const char* access_token,
                                                                     const char* user_uri, const char* etag,
                                                                     cimsue_xcap_doc_t* out);

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
    CIMSUE_STRUCT_COUNT_
} cimsue_struct_id_t;
/** 구조체의 sizeof(이 DLL 의 컴파일 결과). 모르는 id 는 -1. */
CIMSUE_API int32_t CIMSUE_CALL cimsue_struct_size(cimsue_struct_id_t id);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* CIMSUE_C_H */
