// Preview 용 표본 — 화면을 세션 없이 그리기 위한 최소 자료 (android_dispatch_tablet.md §6.3)
//
// SDK 모델(`CallInfo`·`SessionItem` 등)은 기본값이 없는 data class 라 Preview 마다 스무 개 인자를 채워야 한다.
// 여기 한 번만 채워 두고 **바꿀 것만 이름으로 지정**한다 — Preview 가 한 줄로 끝나고, 무엇을 바꿔 보는지가
// 그 한 줄에 드러난다.
//
// 이 파일은 **화면 판정용 표본**이지 시험 자료가 아니다. 단위시험은 자기 자료를 직접 만든다.
package com.cims.ue.dispatch.ui

import com.cims.ue.dispatch.session.AccountKind
import com.cims.ue.dispatch.session.Operation
import com.cims.ue.dispatch.session.SessionItem
import com.cims.ue.sdk.CallDir
import com.cims.ue.sdk.CallInfo
import com.cims.ue.sdk.CallState
import com.cims.ue.sdk.FloorInfo
import com.cims.ue.sdk.FloorState
import com.cims.ue.sdk.McpttCondition
import com.cims.ue.sdk.McpttInfo
import com.cims.ue.sdk.MediaSource

fun previewMcptt(
    present: Boolean = false,
    sessionType: String = "",
    groupId: String = "",
    emergency: Boolean = false,
    privateCall: Boolean = false,
    noFloorCtrl: Boolean = false,
) = McpttInfo(
    present = present, sessionType = sessionType, requestUri = "", callingUserId = "",
    callingGroupId = groupId, emergency = emergency, imminentPeril = false,
    privateCall = privateCall, noFloorCtrl = noFloorCtrl)

fun previewCallInfo(
    callId: Int = 1,
    remoteUri: String = "sip:1001@cims",
    state: CallState = CallState.ACTIVE,
    dir: CallDir = CallDir.OUTGOING,
    calledParty: String = "",
    groupId: String = "",
    isMcptt: Boolean = false,
    listen: Boolean = false,
    listenOnly: Boolean = false,
    joinedDialog: String = "",
    sources: List<MediaSource> = emptyList(),
    mcptt: McpttInfo = previewMcptt(present = isMcptt, groupId = groupId),
) = CallInfo(
    callId = callId, accountId = 0, dir = dir, state = state,
    remoteUri = remoteUri, calledParty = calledParty,
    video = false, mediaActive = true, muted = false, listen = listen,
    playbackRoute = 0, lastCode = 0, lastReason = "",
    sources = sources,
    isMcptt = isMcptt, groupId = groupId, mcptt = mcptt,
    halfDuplex = isMcptt, listenOnly = listenOnly, joinedDialog = joinedDialog,
    condition = McpttCondition(emergency = mcptt.emergency, imminentPeril = mcptt.imminentPeril))

fun previewFloor(
    state: FloorState = FloorState.IDLE,
    canRequest: Boolean = true,
) = FloorInfo(
    state = state, talkers = emptyList(), canRequest = canRequest,
    indicator = 0, queuePosition = 0, localPort = 0, remoteIp = "", remotePort = 0,
    grantedCount = 0, takenCount = 0, denyCount = 0)

/** 살아 있는 세션 하나. `startedAtMs` 를 과거로 밀어 경과 시간이 붙게 한다. */
fun previewSession(
    callId: Int = 1,
    title: String = "",
    info: CallInfo = previewCallInfo(callId = callId),
    floor: FloorInfo? = null,
    speaker: String = "",
    elapsedSec: Long = 74,
    operation: Operation = Operation.DIAL,
    account: AccountKind = AccountKind.PHONE,
) = SessionItem(
    callId = callId, account = account, operation = operation, info = info, floor = floor,
    startedAtMs = System.currentTimeMillis() - elapsedSec * 1000,
    connectedAtMs = System.currentTimeMillis() - elapsedSec * 1000,
    speaker = speaker,
    speakerSinceMs = if (speaker.isNotEmpty()) System.currentTimeMillis() - 14_000 else null,
    title = title)

fun previewSource(label: String, ssrc: Long = 1L, active: Boolean = true) =
    MediaSource(ssrc = ssrc, label = label, active = active, level = 0f)
