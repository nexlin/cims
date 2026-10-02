// 발언 바 — PTT (docs/design/features/android_dispatch_tablet.md §6.3, dispatch_desktop_ui.md §4.1)
//
// **탭 바깥에 고정한다.** 데스크톱은 1920 캔버스에서 ① 이 늘 보이니 발언 바가 ① 안에 있지만, 태블릿은
// [일반통화] 탭으로 옮기면 ① 이 사라진다. 관제사는 전화를 받으면서 무전하므로 위치가 아니라 의도를
// 이식한다 — 두 탭이 같은 바를 쓴다.
//
// PTT 버튼은 **누르고 있는 동안** 발언이다(TS 24.380 Floor Request → Granted → Release).
//   · `Button` 을 쓰지 않는다 — 자체 클릭 처리가 길게 누르기 제스처와 겹쳐 뗌을 놓친다.
//     `Surface` + `pointerInput` 으로 누름·뗌을 직접 받는다.
//   · 대상이 없어도 **버튼은 보인다**. 비활성 버튼은 어두운 테마에서 사라져 «버튼이 없다» 로 읽힌다 —
//     테두리와 문구를 남기고 왜 못 누르는지 옆에 쓴다.
//
// 대상은 **집합**이다 — 카드 ✓ 를 여럿 켜면 한 번의 PTT 로 대상 전부에 floor 를 요청한다(동시 발언 = 단말 팬아웃,
// dispatch_desktop_ui.md §4.1). 칩마다 그 채널의 승인/대기/거부를 따로 보인다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@Composable
fun TalkBar(
    vm: PttChannelsViewModel,
    lockEnabled: Boolean,
    modifier: Modifier = Modifier,
    /** 대상 칩을 눌렀다 — 그 채널 상세를 연다(데스크톱 발언 바와 같다). */
    onOpenChannel: (String) -> Unit = {},
) {
    val targets by vm.targets.collectAsStateWithLifecycle()
    val locked by vm.locked.collectAsStateWithLifecycle()
    val cards by vm.cards.collectAsStateWithLifecycle()

    // 화면이 사라져도 발언은 풀려야 한다 — pointerInput 코루틴이 취소되면 tryAwaitRelease 뒤가 실행되지
    // 않아(AndroidX SuspendingPointerInputFilter.onDetach 가 입력 Job 을 취소) 마이크가 열린 채 남는다.
    // 세션은 Service 수명이라 호는 살아 있으므로, Composable 이 떠날 때 반드시 해제한다.
    DisposableEffect(Unit) { onDispose { vm.releaseAll() } }

    TalkBarContent(
        targets = targets,
        locked = locked,
        anyJoined = cards.any { it.canCheck },
        onDown = { vm.pttDown(lockEnabled) },
        onUp = { vm.pttUp(lockEnabled) },
        onFocus = onOpenChannel,
        onRemove = vm::toggleTarget,
        onClear = vm::clearTargets,
        modifier = modifier)
}

/** 발언 바 본문 — **순수 컴포저블**. 전체 화면 Preview 가 이것을 쓴다. */
@Composable
fun TalkBarContent(
    targets: List<TalkTargetChip>,
    locked: Boolean = false,
    anyJoined: Boolean = false,
    onDown: () -> Unit = {},
    onUp: () -> Unit = {},
    onFocus: (String) -> Unit = {},
    /** 칩의 × — 그 대상 하나만 뺀다(요청해 둔 floor 도 푼다 — `applyTargets`). */
    onRemove: (String) -> Unit = {},
    onClear: () -> Unit = {},
    modifier: Modifier = Modifier,
) {
    val canTalk = targets.isNotEmpty()
    val granted = targets.count { it.granted }
    val speaking = granted > 0
    val requesting = !speaking && targets.any { it.requesting || it.queued }
    // 대상에 내가 연 일제 통화가 있다 — 발언을 놓으면 통화가 끝난다는 것을 버튼이 미리 말한다.
    val broadcast = targets.any { it.broadcastInitiator }
    // 대상이 **전부** 거부됐다 — 버튼이 1초 동안 빨강 «거부» 로 말한다(칩을 보지 않아도 손끝에서 안다 — 데스크톱과 같다)
    val allDenied = canTalk && targets.all { it.denied }
    var showDenied by remember { mutableStateOf(false) }
    LaunchedEffect(allDenied) {
        if (allDenied) { showDenied = true; kotlinx.coroutines.delay(1000); showDenied = false } else showDenied = false
    }

    val p = com.cims.ue.dispatch.ui.Tokens.palette
    Surface(color = p.bar, contentColor = p.ink, modifier = modifier.fillMaxWidth()
        .drawBehind { drawLine(p.divider, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }) {
        Row(
            Modifier.height(80.dp).padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            PttButton(
                enabled = canTalk,
                speaking = speaking,
                requesting = requesting,
                locked = locked,
                denied = showDenied,
                label = when {
                    showDenied -> "거부"
                    speaking && granted < targets.size -> "발언 $granted/${targets.size}"
                    speaking -> "발언 중"
                    requesting -> "요청 중"
                    locked -> "잠금"
                    else -> "PTT"
                },
                hint = when {
                    !canTalk -> "대상 없음"
                    speaking -> if (broadcast) "놓으면 끝납니다" else "말하세요"
                    requesting -> "발언권 요청 중"
                    locked -> "다시 누르면 끝"
                    targets.size > 1 -> "동시 발언 ${targets.size}채널"
                    else -> "누르고 말하기"
                },
                onDown = onDown,
                onUp = onUp)

            if (!canTalk) {
                // 못 누르는 이유를 화면에 쓴다 — 회색 버튼만 두면 «버튼이 없다» 로 읽힌다.
                Column {
                    Text(
                        if (anyJoined) "발언 대상 없음" else "참여한 채널 없음",
                        fontWeight = FontWeight.Bold, fontSize = Type.strong)
                    Text(
                        if (anyJoined) "채널 카드의 ✓ 를 누르세요"
                        else "내 채널에서 [참여] 를 누르세요",
                        fontSize = Type.body, color = p.muted)
                }
            } else {
                // 대상 칩 — 대상별 상태를 따로 본다(하나가 거부돼도 나머지는 살아 있다). 대상에 상한이 없어(동시 발언) 칩이 바를
                //   넘칠 수 있다 — 줄을 가로로 민다(잘려서 안 보이는 대상으로 말이 나가면 안 된다).
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.weight(1f).horizontalScroll(rememberScrollState())
                ) {
                    // 칩 = 그 채널로 포커스, × = 그 대상 하나만 빼기(데스크톱 칩 «클릭 = 포커스 · × = 체크 해제»). 여럿을 잡아 둔
                    //   채 하나만 빼려고 목록으로 돌아갈 필요가 없다.
                    targets.forEach { t ->
                        // 승인된 대상은 발언 색(녹색)으로 채운다 — 여럿 중 어디로 나가고 있는지 칩에서 바로 읽힌다.
                        //   요청·대기 = 주황 테두리 + 옅은 주황 면, 거부·회수 = 빨강(데스크톱 발언 바 칩과 같다).
                        val (bg, fg, line) = when {
                            t.granted -> Triple(p.talkFill, p.onAccent, p.talkFill)
                            t.requesting || t.queued -> Triple(p.ringSoft, p.ink, p.peril)
                            t.denied -> Triple(p.emgSoft, p.emg, p.emg)
                            else -> Triple(p.paper, p.ink, p.edge)
                        }
                        Surface(
                            color = bg, contentColor = fg,
                            shape = RoundedCornerShape(8.dp),
                            border = BorderStroke(if (t.card.emergency) 2.dp else 1.dp, if (t.card.emergency) p.emg else line),
                            modifier = Modifier.height(36.dp).clickable { onFocus(t.card.id) },
                        ) {
                            Row(Modifier.padding(start = 12.dp, end = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                                Text(t.name + (if (t.stateText.isNotEmpty()) " · ${t.stateText}" else ""),
                                     fontSize = Type.body, fontWeight = FontWeight.Bold, maxLines = 1)
                                Icon(Icons.Filled.Close, contentDescription = "${t.name} 발언 대상에서 빼기",
                                    modifier = Modifier.padding(start = 4.dp).size(20.dp).clickable { onRemove(t.card.id) })
                            }
                        }
                    }
                }
                // 남은 발언 — 승인된 대상 중 **가장 적게 남은** 것(먼저 끊기는 채널이 기준이다) + 발언 경과. 발언 중에만 선다.
                if (speaking) TalkGauge(targets.filter { it.granted }.map { it.card })
                TextButton(onClick = onClear) { Text("모두 해제", fontSize = Type.strong, fontWeight = FontWeight.Bold, color = p.ink) }
            }
        }
    }
}

/** 남은 발언 게이지(80×6) + 경과 — 1초마다 다시 그린다(경과는 계산 속성이라 스스로 알리지 않는다). 시한이 임박하면 빨강. */
@Composable
private fun TalkGauge(granted: List<ChannelCard>) {
    val p = com.cims.ue.dispatch.ui.Tokens.palette
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { kotlinx.coroutines.delay(1000); tick++ } }
    @Suppress("UNUSED_EXPRESSION") tick
    val gauge = granted.minOfOrNull { it.talkGauge } ?: 0f
    val near = granted.any { it.talkLimitNear }
    val elapsed = granted.maxOfOrNull { it.speakerElapsedMs } ?: 0L
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Text("남은 발언", fontSize = Type.meta, color = p.muted)
        Box(Modifier.width(80.dp).height(6.dp).drawBehind {
            val r = androidx.compose.ui.geometry.CornerRadius(size.height / 2)
            drawRoundRect(p.line, cornerRadius = r)
            drawRoundRect(if (near) p.emg else p.talk, size = size.copy(width = size.width * gauge.coerceIn(0f, 1f)), cornerRadius = r)
        })
        Text(fmtElapsed(elapsed), fontSize = Type.meta, color = p.muted,
            fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace)
    }
}

/**
 * 누르고 있는 동안 발언하는 큰 버튼.
 *
 * 태블릿은 장갑 낀 손으로도 눌러야 하므로 크게 둔다(데스크톱 40px 의 태블릿 대응). 비활성이어도
 * 테두리로 남겨 **버튼의 존재**는 항상 보인다.
 */
@Composable
internal fun PttButton(
    enabled: Boolean,
    speaking: Boolean,
    requesting: Boolean,
    locked: Boolean,
    label: String,
    onDown: () -> Unit,
    onUp: () -> Unit,
    /** 둘째 줄 — 지금 무엇을 하면 되나(대상 없음 / 누르고 말하기 / 발언권 요청 중 / 말하세요 / 다시 누르면 끝). 비면 상태에서 고른다. */
    hint: String = "",
    /** 방금 전부 거부됐다 — 빨강으로 잠깐 선다. */
    denied: Boolean = false,
    width: androidx.compose.ui.unit.Dp = 150.dp,
    height: androidx.compose.ui.unit.Dp = 64.dp,
    compact: Boolean = false,
) {
    var pressed by remember { mutableStateOf(false) }
    // 제스처 블록은 처음 조성의 람다를 붙잡는다(`pointerInput(Unit)`) — 최신 것을 읽게 한다. 그러지 않으면 설정에서 «잠금 발언» 을
    //   바꿔도 화면 PTT 는 옛 값으로 누르고 뗀다(하드키와 어긋나고, 손을 떼도 마이크가 열린 채 남는다).
    val down by rememberUpdatedState(onDown)
    val up by rememberUpdatedState(onUp)
    val scheme = MaterialTheme.colorScheme
    val p = com.cims.ue.dispatch.ui.Tokens.palette

    // 준비 = 남색 채움, 요청 중·대기열 = 주황, **발언 = 녹색**(손끝에서 «지금 나간다» 를 바로 알게), 비활성 = 띠 + 테두리,
    //   잠금 발언 = 파란 테두리(데스크톱 발언 바 PTT 와 같은 색 규약, dispatch_desktop_ui.md §3.2).
    val bg = when {
        !enabled -> p.bar
        denied -> p.emgFill
        speaking -> p.talkFill
        requesting -> p.peril
        else -> p.primary
    }
    val fg = when {
        !enabled -> p.faint
        speaking -> p.onAccent
        requesting -> p.onPeril
        else -> p.onPrimary
    }

    Surface(
        color = bg,
        contentColor = fg,
        shape = RoundedCornerShape(12.dp),
        border = when {
            !enabled -> BorderStroke(1.dp, p.line)
            locked -> BorderStroke(3.dp, p.held)
            else -> null
        },
        modifier = Modifier
            .width(width)
            .height(height)
            .alpha(if (pressed && enabled && !speaking) 0.85f else 1f)
            .then(
                if (!enabled) Modifier
                else Modifier.pointerInput(Unit) {
                    detectTapGestures(onPress = {
                        pressed = true
                        down()
                        // 취소(제스처 무효화·코루틴 취소)에도 반드시 해제한다 — finally 가 유일한 보장이다.
                        try {
                            tryAwaitRelease()
                        } finally {
                            pressed = false
                            up()
                        }
                    })
                })
    ) {
        Row(
            Modifier.fillMaxSize(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center
        ) {
            Icon(if (enabled) Icons.Filled.Mic else Icons.Filled.MicOff, contentDescription = "발언")
            Spacer(Modifier.width(if (compact) 4.dp else 8.dp))
            if (compact) Text(label, fontWeight = FontWeight.Bold, fontSize = Type.strong)
            else Column {
                Text(label, fontWeight = FontWeight.Bold, fontSize = Type.head)
                Text(
                    hint.ifEmpty { if (!enabled) "대상 없음" else if (speaking) "말하세요" else "누르고 말하기" },
                    fontSize = Type.meta, maxLines = 1)
            }
        }
    }
}
