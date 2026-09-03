// ABI 레이아웃 대조 — C# 구조체 sizeof == DLL 이 컴파일한 cimsue_struct_size(). 헤더와 바인딩의 드리프트를 잡는다.
using CimsUe.Native;
using Xunit;

namespace CimsUe.Tests;

public unsafe class AbiLayoutTests
{
    public static IEnumerable<object[]> Cases() => new object[][]
    {
        new object[] { cimsue_struct_id_t.ENGINE_CONFIG, sizeof(cimsue_engine_config_t) },
        new object[] { cimsue_struct_id_t.ACCOUNT_CONFIG, sizeof(cimsue_account_config_t) },
        new object[] { cimsue_struct_id_t.CALL_OPTIONS, sizeof(cimsue_call_options_t) },
        new object[] { cimsue_struct_id_t.GROUP_CALL_OPTIONS, sizeof(cimsue_group_call_options_t) },
        new object[] { cimsue_struct_id_t.HEADER, sizeof(cimsue_header_t) },
        new object[] { cimsue_struct_id_t.REG_INFO, sizeof(cimsue_reg_info_t) },
        new object[] { cimsue_struct_id_t.MCPTT_INFO, sizeof(cimsue_mcptt_info_t) },
        new object[] { cimsue_struct_id_t.MEDIA_SOURCE, sizeof(cimsue_media_source_t) },
        new object[] { cimsue_struct_id_t.CALL_INFO, sizeof(cimsue_call_info_t) },
        new object[] { cimsue_struct_id_t.TALKER, sizeof(cimsue_talker_t) },
        new object[] { cimsue_struct_id_t.FLOOR_EVENT, sizeof(cimsue_floor_event_t) },
        new object[] { cimsue_struct_id_t.FLOOR_INFO, sizeof(cimsue_floor_info_t) },
        new object[] { cimsue_struct_id_t.REQUEST_RESULT, sizeof(cimsue_request_result_t) },
        new object[] { cimsue_struct_id_t.DIALOG_INFO, sizeof(cimsue_dialog_info_t) },
        new object[] { cimsue_struct_id_t.ROSTER_ENTRY, sizeof(cimsue_roster_entry_t) },
        new object[] { cimsue_struct_id_t.SDS_MESSAGE, sizeof(cimsue_sds_message_t) },
        new object[] { cimsue_struct_id_t.STREAM_STATS, sizeof(cimsue_stream_stats_t) },
        new object[] { cimsue_struct_id_t.AUDIO_DEVICE_INFO, sizeof(cimsue_audio_device_info_t) },
        new object[] { cimsue_struct_id_t.LISTENER, sizeof(cimsue_listener_t) },
        new object[] { cimsue_struct_id_t.CSC_ENDPOINT, sizeof(cimsue_csc_endpoint_t) },
        new object[] { cimsue_struct_id_t.TOKEN_SET, sizeof(cimsue_token_set_t) },
        new object[] { cimsue_struct_id_t.SERVICE_ENDPOINT, sizeof(cimsue_service_endpoint_t) },
        new object[] { cimsue_struct_id_t.SERVICE_PROFILE, sizeof(cimsue_service_profile_t) },
        new object[] { cimsue_struct_id_t.DISPATCH_PROFILE, sizeof(cimsue_dispatch_profile_t) },
        new object[] { cimsue_struct_id_t.PROFILE, sizeof(cimsue_profile_t) },
        new object[] { cimsue_struct_id_t.GROUP_SUMMARY, sizeof(cimsue_group_summary_t) },
        new object[] { cimsue_struct_id_t.XCAP_DOC, sizeof(cimsue_xcap_doc_t) },
    };

    [Theory]
    [MemberData(nameof(Cases))]
    public void StructSizeMatchesNative(object idBoxed, int managedSize)
    {
        var id = (cimsue_struct_id_t)idBoxed;
        Assert.Equal(NativeMethods.cimsue_struct_size(id), managedSize);
    }

    [Fact]
    public void EveryRegisteredStructIsCovered()
    {
        Assert.Equal((int)cimsue_struct_id_t.COUNT_, Cases().Count());
        Assert.Equal(-1, NativeMethods.cimsue_struct_size(cimsue_struct_id_t.COUNT_));
    }

    [Fact]
    public void EnumTextsComeFromCore()
    {
        Assert.Equal("REGISTERED", Engine.ToText(RegState.Registered).ToUpperInvariant());
        Assert.False(string.IsNullOrEmpty(Engine.ToText(FloorEventKind.Granted)));
        Assert.False(string.IsNullOrEmpty(Engine.Version));
    }
}
