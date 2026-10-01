// CimsUe — cimsue_c.h 의 구조체를 그대로 옮긴 blittable 정의 (필드 이름·순서·형이 헤더와 1:1).
//
// 문자열은 byte*(UTF-8, NUL 종료), 배열은 (포인터, 개수), 참/거짓은 int, 열거형은 int — C 의 enum 은 int 다.
// 레이아웃은 LayoutKind.Sequential 의 자연 정렬이 MSVC x64 와 같다. 어긋남은 CimsUe.Tests 의 ABI 시험이
// cimsue_struct_size() 와 대조해 잡는다. 헤더에 필드를 더하면 여기에도 같은 자리에 더한다.
using System.Runtime.InteropServices;

#pragma warning disable IDE1006 // 이름은 C 헤더를 그대로 따른다
namespace CimsUe.Native;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_engine_config_t
{
    public byte* user_agent;
    public int log_level;
    public byte* tls_ca_pem;
    public int tls_verify_server;
    public int null_audio_device;
    public int no_vad;
    public int udp_port, tcp_port, tls_port;
    public uint clock_rate;
    // 끝에 덧붙였다
    public int udp_no_tcp_switch;
    public int grant_mic_delay_ms;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_account_config_t
{
    public byte* server_host;
    public int server_port;
    public int transport;
    public byte* domain;
    public byte* msisdn;
    public byte* imsi;
    public byte* auth_id;
    public byte* display_name;
    public byte* ha1;
    public byte* password;
    public int auth_scheme;
    public byte* aka_k;
    public byte* aka_opc;
    public byte* aka_amf;
    public byte** sec_mechanisms;
    public int sec_mechanism_count;
    public int media_security;
    public int expires_sec;
    public byte* contact_params;
    public int video_auto_transmit;
    public byte* mcptt_id;
    public int auto_answer_mcptt;
    public byte* instance_id;
    // 끝에 덧붙였다(MCPTT·MCData)
    public byte* mcptt_client_id;
    public byte* rp_emergency;
    public byte* rp_imminent_peril;
    public byte* rp_normal;
    public int max_sds_cplane_bytes;
    public int mcdata_msrp;
    public byte* mcptt_server_uri;
    public byte* mcdata_server_uri;
    public int mcvideo_enabled;
    public byte* mcvideo_server_uri;
    public int auto_answer_mcvideo;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_call_options_t
{
    public int video;
    public int emergency;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_call_options_t
{
    public int emergency;
    public int imminent_peril;
    public int listen_only;
    public int full_duplex;
    public byte** members;
    public int member_count;
    public int broadcast;
    public int implicit_floor_request;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_video_group_call_options_t
{
    public int prearranged;
    public int queueing;
    public int max_priority;
    public int max_reception_priority;
    public int implicit_transmission_request;
    public byte* session_uri;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_header_t
{
    public byte* name;
    public byte* value;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_reg_info_t
{
    public int account_id;
    public int state;
    public int code;
    public byte* reason;
    public int expires_sec;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_mcptt_info_t
{
    public int present;
    public byte* session_type;
    public byte* request_uri;
    public byte* calling_user_id;
    public byte* calling_group_id;
    public int emergency;
    public int imminent_peril;
    public int private_call;
    public int no_floor_ctrl;
    public int broadcast;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_mcptt_condition_t
{
    public int emergency;
    public int imminent_peril;
    public int mine;
    public int pending;
    public int last_code;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_emergency_alert_t
{
    public int account_id;
    public byte* group_id;
    public byte* user_id;
    public byte* originated_by;
    public byte* mc_org;
    public int alert_ind;
    public int emergency_ind;
    public int imminent_peril_ind;
    public int self;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_media_source_t
{
    public uint ssrc;
    public byte* label;
    public int active;
    public float level;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_call_info_t
{
    public int call_id;
    public int account_id;
    public int dir;
    public int state;
    public byte* remote_uri;
    public byte* called_party;
    public int video;
    public int media_active;
    public int muted;
    public int listen;
    public int playback_route;
    public int last_code;
    public byte* last_reason;
    public cimsue_media_source_t* sources;
    public int source_count;
    public int is_mcptt;
    public byte* group_id;
    public cimsue_mcptt_info_t mcptt;
    public int half_duplex;
    public int listen_only;
    public byte* joined_dialog;
    // 끝에 덧붙였다
    public float rx_level;
    public cimsue_mcptt_condition_t condition;
    public byte* answer_state;
    public byte** non_ack_users;
    public int non_ack_user_count;
    public int service;
    public byte* session_uri;
    public int video_send;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_talker_t
{
    public byte* id;
    public uint ssrc;
    public int self;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_floor_event_t
{
    public int kind;
    public int call_id;
    public int state;
    public int duration_sec;
    public int cause;
    public byte* cause_text;
    public int indicator;
    public int permission;
    public int queue_position;
    public int me_speaking;
    public cimsue_talker_t* talkers;
    public int talker_count;
    public int raw_type;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_floor_info_t
{
    public int state;
    public cimsue_talker_t* talkers;
    public int talker_count;
    public int can_request;
    public int indicator;
    public int queue_position;
    public int local_port;
    public byte* remote_ip;
    public int remote_port;
    public uint granted_count, taken_count, deny_count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_request_result_t
{
    public int account_id;
    public long token;
    public byte* method;
    public int code;
    public byte* reason;
    public byte* etag;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dialog_info_t
{
    public int account_id;
    public byte* watched;
    public byte* id;
    public byte* call_id;
    public byte* local_tag;
    public byte* remote_tag;
    public byte* direction;
    public byte* state;
    public byte* remote_identity;
    public int full;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_roster_entry_t
{
    public byte* uri;
    public byte* status;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_sds_message_t
{
    public int account_id;
    public byte* from_uri;
    public byte* group_uri;
    public byte* conv_id;
    public byte* msg_id;
    public long time_sec;
    public int disposition_req;
    public byte* text;
    public int notification;
    public int notif_type;
    public int fd;
    public byte* file_url;
    public byte* file_name;
    public byte* file_type;
    public long file_size;
    public int media_plane;    // 끝에 덧붙였다
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_stream_stats_t
{
    public uint rx_packets, rx_bytes, rx_loss, rx_discard;
    public uint tx_packets, tx_bytes;
    public int valid;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_quality_direction_t
{
    public int valid;
    public uint packets, lost, discarded;
    public double loss_pct, discard_pct, jitter_ms, jitter_max_ms;
    public double burst_density_pct, gap_density_pct;
    public int burst_ms, gap_ms;
    public int signal_dbm, noise_dbm;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_call_quality_t
{
    public int valid;
    public byte* codec;
    public uint clock_rate;
    public int wideband;
    public cimsue_quality_direction_t rx;
    public cimsue_quality_direction_t remote;
    public double rtd_ms, esd_ms, one_way_ms;
    public double r_lq, r_cq, mos_lq, mos_cq;
    public long start_epoch_ms, duration_ms;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_tls_peer_expiry_t
{
    public int valid;
    public long not_after_epoch;
    public long observed_epoch;
    public int days_left;
    public byte* subject;
    public byte* remote;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_audio_device_info_t
{
    public int id;
    public byte* name;
    public byte* driver;
    public uint input_count;
    public uint output_count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_video_device_info_t
{
    public int id;
    public byte* name;
    public byte* driver;
    public int capture;
    public int render;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_video_frame_t
{
    public int call_id;
    public int width;
    public int height;
    public int stride;
    public byte* data;
    public long size;
}

/// <summary>Listener 가상함수 1:1 의 함수 포인터 한 벌 + user. 코어 이벤트 스레드에서 호출된다.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_video_transmitter_t
{
    public byte* user_id;
    public uint audio_ssrc;
    public uint video_ssrc;
    public byte* functional_alias;
    public int automatic;
    public int state;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_transmission_event_t
{
    public int kind;
    public int call_id;
    public int state;
    public int cause;
    public byte* cause_text;
    public int duration_sec;
    public int priority;
    public int queue_position;
    public int indicator;
    public uint audio_ssrc, video_ssrc;
    public byte* receiver_id;
    public int raw_type;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_reception_event_t
{
    public int kind;
    public int call_id;
    public cimsue_video_transmitter_t transmitter;
    public int cause;
    public byte* cause_text;
    public int raw_type;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_transmission_info_t
{
    public int state;
    public cimsue_video_transmitter_t* transmitters;
    public int transmitter_count;
    public int queue_position;
    public int local_port;
    public byte* remote_ip;
    public int remote_port;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_listener_t
{
    public void* user;
    public delegate* unmanaged[Cdecl]<void*, int, byte*, void> on_log;
    public delegate* unmanaged[Cdecl]<void*, cimsue_reg_info_t*, void> on_reg_state;
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, void> on_incoming_call;
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, void> on_call_state;
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, void> on_call_media;
    public delegate* unmanaged[Cdecl]<void*, cimsue_floor_event_t*, void> on_floor;
    public delegate* unmanaged[Cdecl]<void*, int, byte*, cimsue_roster_entry_t*, int, int, void> on_roster;
    public delegate* unmanaged[Cdecl]<void*, cimsue_dialog_info_t*, void> on_dialog_info;
    public delegate* unmanaged[Cdecl]<void*, cimsue_sds_message_t*, void> on_sds;
    public delegate* unmanaged[Cdecl]<void*, cimsue_request_result_t*, void> on_request_result;
    public delegate* unmanaged[Cdecl]<void*, int, byte*, byte*, byte*, void> on_message;
    public delegate* unmanaged[Cdecl]<void*, void> on_engine_stopped;
    // 끝에 덧붙였다
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, int, void> on_mcptt_condition;
    public delegate* unmanaged[Cdecl]<void*, cimsue_emergency_alert_t*, void> on_emergency_alert;
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, void> on_non_acknowledged_users;
    public delegate* unmanaged[Cdecl]<void*, cimsue_transmission_event_t*, void> on_transmission;
    public delegate* unmanaged[Cdecl]<void*, cimsue_reception_event_t*, void> on_reception;
    public delegate* unmanaged[Cdecl]<void*, cimsue_video_frame_t*, void> on_video_frame;
}

// ── CSC 설정 평면 (csc.h) ──

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_csc_endpoint_t
{
    public byte* host;
    public int port;
    public byte* client_id;
    public byte* redirect_uri;
    public byte* scope;
    public byte* ca_pem;
    public int verify_server;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_token_set_t
{
    public byte* access_token;
    public byte* token_type;
    public byte* refresh_token;
    public byte* id_token;
    public byte* scope;
    public int expires_in_sec;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_service_endpoint_t
{
    public int transport;
    public int port;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_service_profile_t
{
    public byte* kind;
    public byte* sip_host;
    public int sip_port;
    public int transport;
    public cimsue_service_endpoint_t* transports;
    public int transport_count;
    public int enforced;
    public int media_security;
    public byte* domain;
    public byte* msisdn;
    public byte* imsi;
    public byte* auth_id;
    public byte* sip_ha1;
    public byte* mcptt_id;
    public int auth_scheme;
    public byte* aka_k;
    public byte* aka_opc;
    public byte* aka_amf;
    public byte** sec_mechanisms;
    public int sec_mechanism_count;
    public int max_payload_sds_cplane_bytes;
    // 끝에 덧붙였다
    public int udp_no_tcp_switch;
    public int sms_gateway;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dispatch_member_t
{
    public byte* user_id;
    public byte* name;
    public byte* volte_aor;
    public byte* ptt_id;
    public byte* extension;
    public byte* group_id;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dispatch_target_t
{
    public byte* id;
    public byte* uri;
    public byte* name;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dispatch_profile_t
{
    public int present;
    public byte* group_id;
    public byte* group_name;
    public byte* pilot_id;
    public byte* monitor_scope;
    public byte* ptt_listen;
    public byte* listen_visibility;
    public byte* directory_admin;
    public byte* org_code;
    public cimsue_dispatch_member_t* members;
    public int member_count;
    public cimsue_dispatch_target_t* ptt_targets;
    public int ptt_target_count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_profile_t
{
    public byte* display_name;
    public byte* login_id;
    public byte* country_code;
    public byte* csc_host;
    public int csc_port;
    public cimsue_service_profile_t* services;
    public int service_count;
    public cimsue_dispatch_profile_t dispatch;
    public int allow_group_creation;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_summary_t
{
    public byte* uri;
    public byte* display_name;
    public byte* etag;
    public int member_count;
    public int is_owner;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_xcap_doc_t
{
    public byte* body;
    public byte* etag;
    public int not_modified;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_http_result_t
{
    public int status;
    public byte* content_type;
    public byte* etag;
    public byte* body;
    public int body_len;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_fd_file_t
{
    public byte* url;
    public byte* name;
    public byte* type;
    public long size;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_fd_upload_t
{
    public byte* id;
    public byte* url;
    public byte* name;
    public long size;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_member_t
{
    public byte* uri;
    public byte* display_name;
    public byte* role;
    public int priority;
    public int required;   // 헤더와 같은 순서 — 끝에 덧붙였다(64비트 크기 불변)
    public byte* title;    // 산출 전용(직함) — 끝에 덧붙였다
    public byte* mcvideo_id; // MCVideo entry ID — NULL = uri 와 같다. 끝에 덧붙였다
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_mcvideo_group_attrs_t
{
    public int present;
    public int invite_members;
    public int max_duration_sec;
    public int protect_media;
    public int protect_transmission_control;
    public byte** audio_encodings;
    public int audio_encoding_count;
    public byte** video_encodings;
    public int video_encoding_count;
    public byte* video_resolutions;
    public byte* video_frame_rate;
    public int urgent_real_time_video_mode;
    public int non_urgent_real_time_video_mode;
    public int non_real_time_video_mode;
    public byte* active_real_time_video_mode;
    public int max_transmitters;
    public int min_number_to_start;
    public int group_priority;
    public int reception_hang_timer_sec;
    public int allow_conference_state;
    public int allow_emergency_call;
    public int allow_emergency_alert;
    public int allow_imminent_peril_call;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_doc_t
{
    public byte* uri;
    public byte* display_name;
    public byte* etag;
    public cimsue_group_member_t* members;
    public int member_count;
    public byte* session_type;
    public int encryption;
    public int emergency_call;
    public int emergency_alert;
    public int allow_sds;
    public int allow_fd;
    public int require_affiliation;
    public int priority;
    public int max_participants;
    public byte* org_code;
    public byte* authorized_user;
    // 헤더(cimsue_c.h)와 같은 순서 — 구조체 끝에 덧붙였다. has_* = 0 이면 미기재(0 초기화 = 미기재).
    public int has_hang_timer;
    public int hang_timer_sec;
    public int has_max_duration;
    public int max_duration_sec;
    public int has_conference_state;
    public int allow_conference_state;
    public int has_max_sds_size;
    public int max_sds_size;
    public int has_max_auto_recv;
    public int max_auto_recv;
    // 확인 통화 설정(TS 24.481 §7.2.2 s)t)u)) — 끝에 덧붙였다
    public int has_min_number_to_start;
    public int min_number_to_start;
    public int has_ack_timeout;
    public int ack_timeout_sec;
    public byte* ack_action;
    public cimsue_mcvideo_group_attrs_t mcvideo;   // MCVideo 몫 — present = 0(0 초기화)이면 PUT 에 싣지 않는다. 끝에 덧붙였다
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_cms_entry_t
{
    public byte* uri;
    public byte* mode;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_user_profile_doc_t
{
    public byte* etag;
    public int not_modified;
    public byte* user_uri;
    public cimsue_cms_entry_t emergency_group;
    public cimsue_cms_entry_t imminent_peril_group;
    public cimsue_cms_entry_t emergency_alert_group;
    public cimsue_cms_entry_t emergency_private_recipient;
    public byte** groups;
    public int group_count;
    public byte** implicit_affiliations;
    public int implicit_affiliation_count;
    public int max_affiliations_n2;
    public int allow_private_call;
    public int allow_emergency_group_call;
    public int allow_imminent_peril_call;
    public int allow_activate_emergency_alert;
    public int allow_cancel_emergency_alert;
    public int allow_emergency_private_call;
    public int allow_adhoc_group_call;
    public int allow_cancel_group_emergency;
    public int allow_cancel_imminent_peril;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_service_config_doc_t
{
    public byte* etag;
    public int not_modified;
    public byte* domain;
    public int num_levels_group_hierarchy;
    public int num_levels_user_hierarchy;
    public byte* rp_emergency;
    public byte* rp_imminent_peril;
    public byte* rp_normal;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_mcvideo_user_profile_doc_t
{
    public byte* etag;
    public int not_modified;
    public byte* user_uri;
    public byte* mcvideo_id;
    public byte** groups;
    public int group_count;
    public byte** implicit_affiliations;
    public int implicit_affiliation_count;
    public int max_affiliations_n2;
    public int max_simultaneous_video_streams;
    public int max_simultaneous_calls_n6;
    public cimsue_cms_entry_t emergency_group;
    public cimsue_cms_entry_t imminent_peril_group;
    public cimsue_cms_entry_t emergency_alert_group;
    public int allow_private_call;
    public int allow_emergency_group_call;
    public int allow_emergency_private_call;
    public int allow_imminent_peril_call;
    public int allow_activate_emergency_alert;
    public int allow_revoke_transmit;
    public int allow_remote_ambient_viewing;
    public int allow_local_ambient_viewing;
    public int allow_adhoc_group_call;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_mcvideo_service_config_doc_t
{
    public byte* etag;
    public int not_modified;
    public byte* domain;
    public byte* rp_emergency;
    public byte* rp_imminent_peril;
    public byte* rp_normal;
    public int confidentiality_protection;
    public int integrity_protection;
    public int t100_sec, t101_sec, t102_sec, t103_sec, t104_sec;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_ue_init_config_doc_t
{
    public byte* etag;
    public int not_modified;
    public byte* domain;
    public byte* mcptt_server_uri;
    public byte* mcdata_server_uri;
    public byte* mcvideo_server_uri;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_capabilities_t
{
    public int user_profile_known;
    public int service_config_known;
    public int private_call;
    public int emergency_group_call;
    public int imminent_peril_call;
    public int emergency_private_call;
    public int emergency_alert;
    public int cancel_emergency_alert;
    public int adhoc_group_call;
    public int max_affiliations_n2;
    public int cancel_group_emergency;
    public int cancel_imminent_peril;
}

/// <summary>cimsue_struct_id_t — ABI 자기검사용 구조체 id (헤더와 같은 순서).</summary>
internal enum cimsue_struct_id_t
{
    ENGINE_CONFIG = 0, ACCOUNT_CONFIG, CALL_OPTIONS, GROUP_CALL_OPTIONS, HEADER, REG_INFO, MCPTT_INFO, MEDIA_SOURCE, CALL_INFO,
    TALKER, FLOOR_EVENT, FLOOR_INFO, REQUEST_RESULT, DIALOG_INFO, ROSTER_ENTRY, SDS_MESSAGE, STREAM_STATS, AUDIO_DEVICE_INFO,
    LISTENER, CSC_ENDPOINT, TOKEN_SET, SERVICE_ENDPOINT, SERVICE_PROFILE, DISPATCH_PROFILE, PROFILE, GROUP_SUMMARY, XCAP_DOC,
    DISPATCH_MEMBER, DISPATCH_TARGET, GROUP_MEMBER, GROUP_DOC, HTTP_RESULT, TLS_PEER_EXPIRY, FD_FILE, FD_UPLOAD,
    QUALITY_DIRECTION, CALL_QUALITY,
    MCPTT_CONDITION, EMERGENCY_ALERT, VIDEO_DEVICE_INFO, CMS_ENTRY, USER_PROFILE_DOC, SERVICE_CONFIG_DOC, CAPABILITIES,
    UE_INIT_CONFIG_DOC,
    VIDEO_GROUP_CALL_OPTIONS, VIDEO_TRANSMITTER, TRANSMISSION_EVENT, RECEPTION_EVENT, TRANSMISSION_INFO,
    MCVIDEO_GROUP_ATTRS, MCVIDEO_USER_PROFILE_DOC, MCVIDEO_SERVICE_CONFIG_DOC,
    VIDEO_FRAME,
    COUNT_,
}
