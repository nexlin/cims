// PTT 그룹 편집 폼 ↔ GMS 그룹 문서 왕복 (docs/design/features/android_dispatch_tablet.md §6.12,
// dispatch_desktop_ui.md §4.7 «폼의 그룹 호·한도 칸» · §10.6 «서비스» 절)
//
// 여기서 지키는 것은 **저장이 서버 값을 망가뜨리지 않는다**는 것이다. XCAP PUT 은 문서를 통째로 갈아끼우므로
//   · 폼을 열었다 닫은 것만으로 미기재 칸이 명시값으로 굳거나,
//   · 콘솔이 준 멤버별 우선순위·필수 표시가 기본값으로 덮이거나,
//   · 폼에 없는 MCVideo 속성(코덱·해상도)이 사라지면
// 관제 앱의 저장 한 번이 콘솔 설정을 지운다. 데스크톱 `GroupEditViewModel` 과 같은 규칙인지 본다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.GroupInfo
import com.cims.ue.dispatch.session.ManagedGroup
import com.cims.ue.dispatch.ui.groups.EditForm
import com.cims.ue.dispatch.ui.groups.GroupDefaults
import com.cims.ue.dispatch.ui.groups.MemberRow
import com.cims.ue.dispatch.ui.groups.PttGroupsViewModel
import com.cims.ue.dispatch.ui.groups.capabilityChips
import com.cims.ue.dispatch.ui.groups.editFormOf
import com.cims.ue.dispatch.ui.groups.emergencyText
import com.cims.ue.dispatch.ui.groups.groupDocOf
import com.cims.ue.dispatch.ui.groups.groupListHint
import com.cims.ue.dispatch.ui.groups.mcVideoText
import com.cims.ue.dispatch.ui.groups.ownerLabel
import com.cims.ue.dispatch.ui.groups.sessionTypeText
import com.cims.ue.sdk.GroupDoc
import com.cims.ue.sdk.GroupMember
import com.cims.ue.sdk.McVideoGroupAttrs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GroupFormTest {

    private val members = listOf(
        GroupMember("tel:1001", "김관제", role = "chair", priority = 12, required = true),
        GroupMember("tel:1002", "이당직", role = "participant", priority = 9, mcvideoId = "tel:v1002"),
        GroupMember("tel:1003", "박현장", role = "participant", priority = 5))

    /** 그룹 호·한도 칸이 전부 미기재인 문서 — 구 서버·콘솔에서 손대지 않은 그룹. */
    private val bare = GroupDoc(uri = "tel:g1", displayName = "순찰1", etag = "W/\"e1\"", members = members)

    private val row = ManagedGroup(id = "g1", uri = "tel:g1", name = "순찰1", etag = "W/\"list\"")

    private fun open(d: GroupDoc): EditForm = editFormOf(d, row, myPttNumber = "1001")

    // ── 미기재 보존 ──

    @Test fun `폼을 열었다 그대로 저장하면 미기재 칸은 미기재로 남는다`() {
        val out = groupDocOf(open(bare))
        assertNull(out.hangTimerSec)
        assertNull(out.maxDurationSec)
        assertNull(out.minNumberToStart)
        assertNull(out.ackTimeoutSec)
        assertNull(out.ackAction)
        assertNull(out.allowConferenceState)
        assertNull(out.maxSdsSize)
        assertNull(out.maxAutoRecv)
        assertNull(out.mcvideo)
    }

    @Test fun `미기재 칸은 기본값으로 보인다`() {
        val f = open(bare)
        assertEquals(30, f.hangTimerSec)
        assertEquals(3600, f.maxDurationSec)
        assertEquals(0, f.minNumberToStart)
        assertEquals(5, f.ackTimeoutSec)
        assertEquals("abandon", f.ackAction)
        assertTrue(f.allowConferenceState)
        assertEquals(10000, f.maxSdsSize)
        assertEquals(1048576, f.maxAutoRecv)
    }

    @Test fun `값을 바꾸면 그 값을 싣는다`() {
        val out = groupDocOf(open(bare).copy(hangTimerSec = 10, maxDurationSec = 0, minNumberToStart = 2,
            ackTimeoutSec = 20, maxSdsSize = 0, maxAutoRecv = 4096))
        assertEquals(10, out.hangTimerSec)
        assertEquals(0, out.maxDurationSec)               // 0 = 무제한 — 기본값(3600)과 다르니 싣는다
        assertEquals(2, out.minNumberToStart)
        assertEquals(20, out.ackTimeoutSec)
        assertEquals(0, out.maxSdsSize)
        assertEquals(4096, out.maxAutoRecv)
    }

    @Test fun `범위 밖 값은 범위로 자른다`() {
        val out = groupDocOf(open(bare).copy(hangTimerSec = 99999, maxDurationSec = 999999, minNumberToStart = 70000,
            ackTimeoutSec = 0, priority = 99, maxParticipants = -3))
        assertEquals(3600, out.hangTimerSec)
        assertEquals(86400, out.maxDurationSec)
        assertEquals(65535, out.minNumberToStart)
        assertEquals(1, out.ackTimeoutSec)                // TNG1 은 1~300
        assertEquals(15, out.priority)
        assertEquals(0, out.maxParticipants)
        assertEquals(300, groupDocOf(open(bare).copy(ackTimeoutSec = 301)).ackTimeoutSec)
    }

    @Test fun `문서에 있던 칸은 기본값과 같아도 명시값으로 남는다`() {
        val d = bare.copy(hangTimerSec = 30, maxDurationSec = 3600, minNumberToStart = 0, ackTimeoutSec = 5,
            ackAction = "abandon", allowConferenceState = true, maxSdsSize = 10000, maxAutoRecv = 1048576)
        val out = groupDocOf(open(d))
        assertEquals(30, out.hangTimerSec)
        assertEquals(3600, out.maxDurationSec)
        assertEquals(0, out.minNumberToStart)
        assertEquals(5, out.ackTimeoutSec)
        assertEquals("abandon", out.ackAction)
        assertEquals(true, out.allowConferenceState)
        assertEquals(10000, out.maxSdsSize)
        assertEquals(1048576, out.maxAutoRecv)
    }

    @Test fun `문서에 있던 칸을 기본값으로 되돌려도 지우지 않고 그 값을 싣는다`() {
        val out = groupDocOf(open(bare.copy(hangTimerSec = 120)).copy(hangTimerSec = 30))
        assertEquals(30, out.hangTimerSec)
    }

    @Test fun `참가자 정보 구독 — 미기재였고 켜진 채면 미기재, 끄면 false`() {
        assertNull(groupDocOf(open(bare)).allowConferenceState)
        assertEquals(false, groupDocOf(open(bare).copy(allowConferenceState = false)).allowConferenceState)
        // 문서가 false 였으면 켰을 때 true 를 싣는다(미기재로 떨어뜨리면 서버가 옛 값을 유지한다)
        assertEquals(true,
            groupDocOf(open(bare.copy(allowConferenceState = false)).copy(allowConferenceState = true)).allowConferenceState)
    }

    @Test fun `대기 만료 동작 — 미기재였고 abandon 이면 미기재, proceed 는 싣는다`() {
        assertNull(groupDocOf(open(bare)).ackAction)
        assertEquals("proceed", groupDocOf(open(bare).copy(ackAction = "proceed")).ackAction)
        assertEquals("proceed", open(bare.copy(ackAction = "proceed")).ackAction)
        // 모르는 값은 기본 동작으로 읽는다
        assertEquals("abandon", open(bare.copy(ackAction = "기타")).ackAction)
    }

    @Test fun `새 그룹 — 그룹 호·한도 칸은 전부 미기재, uri 는 tel 정규형`() {
        val f = EditForm(isNew = true, uri = "tel:g-abc", groupId = " g-new ", name = " 3번 게이트 ",
            members = listOf(MemberRow("tel:1001", "김관제", "1001", isChair = true, isMe = true)), loaded = true)
        val out = groupDocOf(f)
        assertEquals("tel:g-new", out.uri)                // 폼의 id 가 정본(신규만 고칠 수 있다)
        assertEquals("3번 게이트", out.displayName)
        assertNull(out.hangTimerSec); assertNull(out.maxDurationSec); assertNull(out.ackAction)
        assertNull(out.maxSdsSize); assertNull(out.maxAutoRecv); assertNull(out.allowConferenceState)
        assertNull(out.mcvideo)
    }

    @Test fun `기존 그룹의 uri·ETag 는 문서가 정본이고 비면 목록 행으로 채운다`() {
        assertEquals("tel:g1", open(bare).uri)
        assertEquals("W/\"e1\"", open(bare).ifMatch)
        val f = open(bare.copy(uri = "", etag = ""))
        assertEquals("tel:g1", f.uri)
        assertEquals("g1", f.groupId)
        assertEquals("W/\"list\"", f.ifMatch)
        assertEquals("tel:g1", groupDocOf(f.copy(groupId = "딴값")).uri)   // 기존 그룹은 id 칸이 uri 를 바꾸지 못한다
    }

    // ── 멤버 — 우선순위·필수·MCVideo ID 보존 ──

    @Test fun `역할을 바꾸지 않은 멤버는 읽은 우선순위를 그대로 보낸다`() {
        val out = groupDocOf(open(bare))
        assertEquals(listOf(12, 9, 5), out.members.map { it.priority })
        assertEquals(listOf("chair", "participant", "participant"), out.members.map { it.role })
    }

    @Test fun `역할을 바꾼 멤버만 역할 기본값을 쓴다 — 의장 7 참가자 5`() {
        val f = open(bare)
        val swapped = f.copy(members = f.members.map {
            when (it.number) { "1001", "1002" -> it.copy(isChair = !it.isChair); else -> it }
        })
        val out = groupDocOf(swapped)
        assertEquals(5, out.members[0].priority)          // 의장 → 참가자
        assertEquals(7, out.members[1].priority)          // 참가자 → 의장
        assertEquals(5, out.members[2].priority)          // 손대지 않음 — 읽은 값(우연히 5)
    }

    @Test fun `역할을 바꿨다가 되돌리면 읽은 우선순위가 돌아온다`() {
        val m = open(bare).members[0]
        assertEquals(12, m.priority)
        assertEquals(5, m.copy(isChair = false).priority)
        assertEquals(12, m.copy(isChair = false).copy(isChair = true).priority)
    }

    @Test fun `새로 더한 멤버는 역할 기본값이다`() {
        assertEquals(5, MemberRow("tel:2001", "신입", "2001").priority)
        assertEquals(7, MemberRow("tel:2001", "신입", "2001", isChair = true).priority)
        val f = open(bare)
        val out = groupDocOf(f.copy(members = f.members + MemberRow("tel:2001", "신입", "2001")))
        assertEquals(listOf(12, 9, 5, 5), out.members.map { it.priority })
        assertFalse(out.members.last().required)
    }

    @Test fun `필수 표시와 MCVideo ID 는 읽은 값을 되돌린다`() {
        val out = groupDocOf(open(bare))
        assertEquals(listOf(true, false, false), out.members.map { it.required })
        assertEquals("tel:v1002", out.members[1].mcvideoId)
        assertEquals("", out.members[0].mcvideoId)
    }

    @Test fun `필수 토글은 그 멤버에만 실린다`() {
        val f = open(bare)
        val out = groupDocOf(f.copy(members = f.members.map { if (it.number == "1003") it.copy(required = true) else it }))
        assertEquals(listOf(true, false, true), out.members.map { it.required })
        assertEquals(listOf(12, 9, 5), out.members.map { it.priority })   // 필수를 바꿔도 우선순위는 그대로
    }

    @Test fun `나 표시는 번호 정규형으로 맞춘다`() {
        val d = bare.copy(members = listOf(GroupMember("tel:+821012345678", "김관제", role = "chair")))
        assertTrue(editFormOf(d, row, myPttNumber = "01012345678").members.single().isMe)
        assertFalse(editFormOf(d, row, myPttNumber = "").members.single().isMe)
    }

    @Test fun `문서에 이름이 없는 멤버는 주소록 이름으로 채운다`() {
        val d = bare.copy(members = listOf(GroupMember("tel:1009")))
        assertEquals("오경비", editFormOf(d, row, "1001") { if (it == "1009") "오경비" else "" }.members.single().name)
        assertEquals("1009", editFormOf(d, row, "1001").members.single().label)
    }

    // ── 서비스 — MCVideo ──

    @Test fun `MCVideo 를 켜지 않은 그룹은 그 몫을 싣지 않는다`() {
        val f = open(bare)
        assertFalse(f.mcVideo); assertFalse(f.mcVideoLocked)
        assertNull(groupDocOf(f).mcvideo)
    }

    @Test fun `새로 켜면 서버 기본 코덱과 보호 false 를 명시하고 나머지는 미기재다`() {
        val mv = groupDocOf(open(bare).copy(mcVideo = true)).mcvideo
        assertNotNull(mv)
        assertEquals(listOf("AMR-WB"), mv!!.audioEncodings)
        assertEquals(listOf("H264"), mv.videoEncodings)
        assertFalse(mv.protectMedia)                      // 요소가 없으면 true 로 읽힌다 — false 를 명시한다
        assertFalse(mv.protectTransmissionControl)
        assertFalse(mv.inviteMembers)                     // chat 이 기본
        assertNull(mv.maxTransmitters); assertNull(mv.maxDurationSec); assertNull(mv.receptionHangTimerSec)
        assertNull(mv.minNumberToStart); assertNull(mv.groupPriority); assertNull(mv.allowConferenceState)
    }

    @Test fun `이번 편집에서 켠 것은 저장 전까지 되돌릴 수 있다`() {
        val on = open(bare).copy(mcVideo = true)
        assertFalse(on.mcVideoLocked)
        assertNull(groupDocOf(on.copy(mcVideo = false)).mcvideo)
    }

    @Test fun `이미 켜진 그룹은 잠기고 폼에 없는 속성을 되돌린다`() {
        val read = McVideoGroupAttrs(inviteMembers = true, maxDurationSec = 1800, protectMedia = true,
            protectTransmissionControl = true, audioEncodings = listOf("AMR-WB", "EVS"), videoEncodings = listOf("H265"),
            videoResolutions = "1280x720", videoFrameRate = "30", urgentRealTimeVideoMode = true,
            activeRealTimeVideoMode = "urgent", maxTransmitters = 4, groupPriority = 200,
            allowEmergencyCall = true, allowImminentPerilCall = false)
        val f = open(bare.copy(mcvideo = read))
        assertTrue(f.mcVideo); assertTrue(f.mcVideoLocked)
        assertEquals("prearranged", f.mcVideoType)
        assertEquals(4, f.mcVideoMaxTransmitters)
        assertEquals(1800, f.mcVideoMaxDurationSec)
        assertEquals(30, f.mcVideoReceptionHangSec)       // 미기재 — 기본값으로 보인다
        assertEquals("200", f.mcVideoGroupPriority)

        val mv = groupDocOf(f).mcvideo!!
        assertEquals(listOf("AMR-WB", "EVS"), mv.audioEncodings)
        assertEquals(listOf("H265"), mv.videoEncodings)
        assertEquals("1280x720", mv.videoResolutions)
        assertEquals("30", mv.videoFrameRate)
        assertEquals(true, mv.urgentRealTimeVideoMode)
        assertEquals("urgent", mv.activeRealTimeVideoMode)
        assertEquals(true, mv.allowEmergencyCall)
        assertEquals(false, mv.allowImminentPerilCall)
        assertTrue(mv.inviteMembers)
        assertEquals(4, mv.maxTransmitters)
        assertEquals(1800, mv.maxDurationSec)
        assertNull(mv.receptionHangTimerSec)              // 미기재였고 기본값 그대로
        assertEquals(200, mv.groupPriority)
        assertFalse(mv.protectMedia)                      // 종단간 보호는 지원하지 않는다 — 늘 false 명시
        assertFalse(mv.protectTransmissionControl)
    }

    @Test fun `MCVideo 칸 — 바꾼 값을 범위로 잘라 싣는다`() {
        val f = open(bare).copy(mcVideo = true, mcVideoType = "prearranged", mcVideoMaxTransmitters = 40,
            mcVideoMaxDurationSec = 999999, mcVideoReceptionHangSec = 7200, mcVideoMinNumberToStart = 3,
            mcVideoAllowConferenceState = false)
        val mv = groupDocOf(f).mcvideo!!
        assertTrue(mv.inviteMembers)
        assertEquals(16, mv.maxTransmitters)              // 1~16
        assertEquals(86400, mv.maxDurationSec)
        assertEquals(3600, mv.receptionHangTimerSec)
        assertEquals(3, mv.minNumberToStart)
        assertEquals(false, mv.allowConferenceState)
        assertEquals(1, groupDocOf(f.copy(mcVideoMaxTransmitters = 0)).mcvideo!!.maxTransmitters)
    }

    @Test fun `MCVideo 그룹 우선순위 — 비우면 미기재, 값은 0~255`() {
        val on = open(bare).copy(mcVideo = true)
        assertNull(groupDocOf(on.copy(mcVideoGroupPriority = " ")).mcvideo!!.groupPriority)
        assertEquals(0, groupDocOf(on.copy(mcVideoGroupPriority = "0")).mcvideo!!.groupPriority)
        assertEquals(255, groupDocOf(on.copy(mcVideoGroupPriority = "999")).mcvideo!!.groupPriority)
        // 읽은 값이 있어도 칸을 비우면 미기재로 되돌린다(가장 낮은 우선순위)
        val read = open(bare.copy(mcvideo = McVideoGroupAttrs(groupPriority = 10)))
        assertNull(groupDocOf(read.copy(mcVideoGroupPriority = "")).mcvideo!!.groupPriority)
    }

    // ── 저장 자격·버튼 글자 ──

    @Test fun `이름과 멤버가 있어야 저장한다 — 신규는 id 도`() {
        val f = open(bare)
        assertTrue(f.canSave)
        assertFalse(f.copy(name = " ").canSave)
        assertFalse(f.copy(members = emptyList()).canSave)
        assertFalse(f.copy(busy = true).canSave)
        assertFalse(EditForm(isNew = false, name = "순찰1", busy = true).canSave)   // 문서를 받는 중
        val new = EditForm(isNew = true, groupId = "", name = "새", members = f.members, loaded = true)
        assertFalse(new.canSave)
        assertTrue(new.copy(groupId = "g-1").canSave)
    }

    @Test fun `저장 버튼 글자`() {
        assertEquals("저장", open(bare).saveLabel)
        assertEquals("그룹 만들기", EditForm(isNew = true, loaded = true).saveLabel)
        assertEquals("저장 중…", open(bare).copy(busy = true).saveLabel)
    }

    // ── 상세 카드의 문구 ──

    @Test fun `능력 칩 — 문서가 주는 것만 선다`() {
        assertEquals(listOf("MCPTT 음성", "메시지(SDS)", "affiliation 필요"), capabilityChips(bare))
        val full = bare.copy(allowFd = true, encryption = true,
            mcvideo = McVideoGroupAttrs(inviteMembers = false, maxTransmitters = 2))
        assertEquals(listOf("MCPTT 음성", "메시지(SDS)", "파일(FD)", "MCVideo chat · 송출 2", "암호화", "affiliation 필요"),
            capabilityChips(full))
        assertEquals(listOf("MCPTT 음성"), capabilityChips(bare.copy(allowSds = false, requireAffiliation = false)))
    }

    @Test fun `MCVideo 문구 — chat 과 편성, 송출 상한은 있을 때만`() {
        assertEquals("", mcVideoText(null))
        assertEquals("MCVideo chat", mcVideoText(McVideoGroupAttrs()))
        assertEquals("MCVideo chat · 송출 2", mcVideoText(McVideoGroupAttrs(maxTransmitters = 2)))
        assertEquals("MCVideo 편성 · 송출 4", mcVideoText(McVideoGroupAttrs(inviteMembers = true, maxTransmitters = 4)))
    }

    @Test fun `긴급 문구`() {
        assertEquals("긴급 통화 · 긴급 경보", emergencyText(bare))
        assertEquals("긴급 경보", emergencyText(bare.copy(emergencyCall = false)))
        assertEquals("허용 안 함", emergencyText(bare.copy(emergencyCall = false, emergencyAlert = false)))
    }

    @Test fun `세션 종류 문구`() {
        assertEquals("사전편성(prearranged)", sessionTypeText("prearranged"))
        assertEquals("채팅(chat)", sessionTypeText("chat"))
        assertEquals("사전편성(prearranged)", sessionTypeText(""))
    }

    @Test fun `소유자 — 내 것은 이름(나), 남의 것은 주소록 이름, 없으면 번호`() {
        val book = mapOf("1002" to "이당직")
        val nameOf = { n: String -> book[n].orEmpty() }
        assertEquals("—", ownerLabel("", "tel:1001", "김관제", nameOf))
        assertEquals("김관제(나)", ownerLabel("tel:1001", "tel:1001", "김관제", nameOf))
        assertEquals("이당직", ownerLabel("sip:1002@cims", "tel:1001", "김관제", nameOf))
        assertEquals("1003", ownerLabel("tel:1003", "tel:1001", "김관제", nameOf))
        // 표기가 달라도 같은 번호면 나다
        assertEquals("김관제(나)", ownerLabel("tel:+821012345678", "sip:01012345678@cims", "김관제", nameOf))
        // 내 번호를 모르면 아무도 나로 표시하지 않는다
        assertEquals("1003", ownerLabel("tel:1003", "", "김관제", nameOf))
    }

    @Test fun `목록 범위 안내`() {
        assertEquals("", groupListHint(emptyList()))
        assertEquals("관리 범위 안 그룹 전부", groupListHint(listOf(row, row.copy(id = "g2"))))
        assertEquals("관리 가능 1개 · 나머지는 청취 범위·멤버 그룹(보기만)",
            groupListHint(listOf(row, row.copy(id = "g2", canManage = false, inListenScope = true))))
    }

    @Test fun `관리 범위가 없으면 세션의 멤버 그룹이 목록이고 내 소유만 고친다`() {
        val rows = PttGroupsViewModel.managedOf(listOf(
            GroupInfo("g1", "tel:g1", "순찰1", memberCount = 4, isOwner = true, etag = "e1", sessionType = "chat"),
            GroupInfo("g2", "tel:g2", "상황실", memberCount = 8),
            GroupInfo("g9", "tel:g9", "청취 범위", isMember = false)))      // 청취 범위 그룹은 관리 범위가 있을 때만 서버가 준다
        assertEquals(listOf("g1", "g2"), rows.map { it.id })
        assertTrue(rows[0].canManage); assertTrue(rows[0].isOwner)
        assertFalse(rows[1].canManage)
        assertTrue(rows.all { it.isMember && it.hasChannel })
        assertEquals("멤버", rows[0].relation)
        assertEquals("e1", rows[0].etag)
        assertEquals("chat", rows[0].sessionType)
    }

    @Test fun `기본값은 콘솔 그룹 편집과 같다`() {
        assertEquals(30, GroupDefaults.HANG_TIMER)
        assertEquals(3600, GroupDefaults.MAX_DURATION)
        assertEquals(10000, GroupDefaults.MAX_SDS_SIZE)
        assertEquals(1048576, GroupDefaults.MAX_AUTO_RECV)
        assertEquals(5, GroupDefaults.ACK_TIMEOUT)
        assertEquals(2, GroupDefaults.MCV_MAX_TRANSMITTERS)
        assertEquals(30, GroupDefaults.MCV_RECEPTION_HANG)
    }
}
