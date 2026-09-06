// CimsUe — cimsue_c.h 의 P/Invoke 선언 (함수 이름·순서 = 헤더). 전부 internal — 앱은 파사드 클래스만 본다.
//
// x64 는 호출 규약이 하나(__cdecl 표기와 같다). 문자열 입력은 UTF-8(LPUTF8Str — 호출 동안만 유효, 코어가 보관하지 않는다는
// 헤더 규약과 맞다). 구조체는 포인터로 넘겨 런타임 마샬링을 거치지 않는다(blittable). 산출 문자열 포인터는 호출자가 곧바로
// 관리 문자열로 복사한다(스레드별 스냅샷 규약 — 다음 조회 전까지만 유효).
using System.Runtime.InteropServices;

#pragma warning disable IDE1006, SYSLIB1054 // 이름은 C 헤더 그대로; DllImport 는 의도(LibraryImport 는 LPUTF8Str+포인터 혼용 시 이점 없음)
namespace CimsUe.Native;

internal static unsafe class NativeMethods
{
    /// <summary>모듈 이름 — 실제 경로는 <see cref="NativeLoader"/> 가 해석한다.</summary>
    public const string Lib = "cimsue";
    private const CallingConvention CC = CallingConvention.Cdecl;
    private const UnmanagedType U8 = UnmanagedType.LPUTF8Str;

    // ── 엔진 ──
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern IntPtr cimsue_engine_create();
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_engine_destroy(IntPtr e);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_engine_config_default(cimsue_engine_config_t* cfg);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_start(IntPtr e, cimsue_engine_config_t* cfg, cimsue_listener_t* listener);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_engine_stop(IntPtr e);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_running(IntPtr e);

    // 계정
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_account_config_default(cimsue_account_config_t* cfg);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_add_account(IntPtr e, cimsue_account_config_t* cfg);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_register_account(IntPtr e, int account_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_unregister_account(IntPtr e, int account_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_refresh_registration(IntPtr e, int account_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_remove_account(IntPtr e, int account_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_engine_reg_info(IntPtr e, int account_id, cimsue_reg_info_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_accounts(IntPtr e, int** @out);

    // 호 (VoLTE 1:1)
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_call_options_default(cimsue_call_options_t* opts);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_dial(IntPtr e, int account_id, [MarshalAs(U8)] string target, cimsue_call_options_t* opts);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_answer(IntPtr e, int call_id, cimsue_call_options_t* opts);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_reject(IntPtr e, int call_id, int status_code);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_hangup(IntPtr e, int call_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_hold(IntPtr e, int call_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_resume(IntPtr e, int call_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_set_muted(IntPtr e, int call_id, int muted);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_set_listen(IntPtr e, int call_id, int listen);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_set_rx_level(IntPtr e, int call_id, float level);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_send_dtmf(IntPtr e, int call_id, [MarshalAs(U8)] string digits);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_engine_call_info(IntPtr e, int call_id, cimsue_call_info_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_calls(IntPtr e, int** @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_engine_stream_stats(IntPtr e, int call_id, cimsue_stream_stats_t* @out);

    // MCPTT 그룹콜·사설콜
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_group_call_options_default(cimsue_group_call_options_t* opts);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_join_group_call(IntPtr e, int account_id, [MarshalAs(U8)] string group_id, cimsue_group_call_options_t* opts);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_start_private_call(IntPtr e, int account_id, [MarshalAs(U8)] string peer, cimsue_group_call_options_t* opts);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_leave_group_call(IntPtr e, int call_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_floor_request(IntPtr e, int call_id, int priority);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_floor_release(IntPtr e, int call_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_floor_queue_cancel(IntPtr e, int call_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_engine_floor_info(IntPtr e, int call_id, cimsue_floor_info_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern long cimsue_engine_affiliate(IntPtr e, int account_id, [MarshalAs(U8)] string group_id, int on);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_subscribe_conference(IntPtr e, int account_id, [MarshalAs(U8)] string group_id, int on);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_subscribe_xcap_diff(IntPtr e, int account_id, [MarshalAs(U8)] string psi_uri, int on);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern long cimsue_engine_send_request(IntPtr e, int account_id, [MarshalAs(U8)] string method, [MarshalAs(U8)] string target_uri, [MarshalAs(U8)] string content_type, [MarshalAs(U8)] string body, cimsue_header_t* headers, int header_count);

    // 관제
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_dialog_watch(IntPtr e, int account_id, [MarshalAs(U8)] string target_aor, int on);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_join(IntPtr e, int account_id, [MarshalAs(U8)] string target_uri, cimsue_dialog_info_t* dlg);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_pickup(IntPtr e, int account_id, [MarshalAs(U8)] string feature_code, [MarshalAs(U8)] string? number);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_transfer(IntPtr e, int call_id, [MarshalAs(U8)] string target);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_transfer_attended(IntPtr e, int call_id, int consult_call_id);

    // MCData SDS
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_send_group_sds(IntPtr e, int account_id, [MarshalAs(U8)] string group_id, [MarshalAs(U8)] string text, int request_delivery, byte* msg_id_out, int msg_id_cap, long* token_out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_send_sds_notification(IntPtr e, int account_id, [MarshalAs(U8)] string peer, [MarshalAs(U8)] string conv_id, [MarshalAs(U8)] string msg_id, int notif_type, long* token_out);

    // 장치
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_audio_devices(IntPtr e, cimsue_audio_device_info_t** @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_refresh_audio_devices(IntPtr e);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_set_audio_devices(IntPtr e, int capture_dev, int playback_dev);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_add_playback_route(IntPtr e, int playback_dev);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_remove_playback_route(IntPtr e, int route_id);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_engine_set_call_route(IntPtr e, int call_id, int route_id);

    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern byte* cimsue_version();
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern byte* cimsue_last_error();

    // toString
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern byte* cimsue_reg_state_str(int s);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern byte* cimsue_call_state_str(int s);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern byte* cimsue_transport_str(int t);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern byte* cimsue_floor_state_str(int s);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern byte* cimsue_floor_kind_str(int k);

    // 문자열 산출 헬퍼 — out 에 최대 cap(NUL 포함) 기록, NUL 제외 길이 반환(cap 이상이면 잘림). out=NULL·cap=0 이면 길이만.
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_account_config_aor(cimsue_account_config_t* cfg, byte* @out, int cap);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_account_config_mcptt_id(cimsue_account_config_t* cfg, byte* @out, int cap);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_account_config_digest_username(cimsue_account_config_t* cfg, byte* @out, int cap);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_account_config_is_complete(cimsue_account_config_t* cfg);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_dialog_info_join_header(cimsue_dialog_info_t* d, byte* @out, int cap);

    // ── CSC 설정 평면 ──
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_csc_endpoint_default(cimsue_csc_endpoint_t* ep);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern IntPtr cimsue_csc_create(cimsue_csc_endpoint_t* ep);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_csc_destroy(IntPtr c);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_login(IntPtr c, [MarshalAs(U8)] string user_name, [MarshalAs(U8)] string password, cimsue_token_set_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_refresh(IntPtr c, [MarshalAs(U8)] string refresh_token, cimsue_token_set_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_fetch_profile(IntPtr c, [MarshalAs(U8)] string access_token, cimsue_profile_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_list_groups(IntPtr c, [MarshalAs(U8)] string access_token, [MarshalAs(U8)] string user_uri, cimsue_group_summary_t** @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_xcap_get(IntPtr c, [MarshalAs(U8)] string access_token, [MarshalAs(U8)] string path, [MarshalAs(U8)] string accept, [MarshalAs(U8)] string? if_none_match, cimsue_xcap_doc_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_get_user_profile(IntPtr c, [MarshalAs(U8)] string access_token, [MarshalAs(U8)] string user_uri, [MarshalAs(U8)] string? etag, cimsue_xcap_doc_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_get_service_config(IntPtr c, [MarshalAs(U8)] string access_token, [MarshalAs(U8)] string user_uri, [MarshalAs(U8)] string? etag, cimsue_xcap_doc_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_get_group(IntPtr c, [MarshalAs(U8)] string access_token, [MarshalAs(U8)] string user_uri, [MarshalAs(U8)] string group_uri, cimsue_group_doc_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_put_group(IntPtr c, [MarshalAs(U8)] string access_token, [MarshalAs(U8)] string user_uri, cimsue_group_doc_t* doc, [MarshalAs(U8)] string? if_match, cimsue_group_doc_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_delete_group(IntPtr c, [MarshalAs(U8)] string access_token, [MarshalAs(U8)] string user_uri, [MarshalAs(U8)] string group_uri);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_group_doc_to_xml(cimsue_group_doc_t* doc, byte* @out, int cap);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_group_doc_parse([MarshalAs(U8)] string xml, cimsue_group_doc_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_parse_profile([MarshalAs(U8)] string json, cimsue_profile_t* @out);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_csc_enc([MarshalAs(U8)] string s, byte* @out, int cap);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern cimsue_service_profile_t* cimsue_profile_service(cimsue_profile_t* profile, [MarshalAs(U8)] string kind);
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern void cimsue_service_profile_to_account(cimsue_service_profile_t* sp, [MarshalAs(U8)] string? login_pw, cimsue_account_config_t* @out);

    // ── ABI 자기검사 ──
    [DllImport(Lib, CallingConvention = CC, ExactSpelling = true)] public static extern int cimsue_struct_size(cimsue_struct_id_t id);
}
