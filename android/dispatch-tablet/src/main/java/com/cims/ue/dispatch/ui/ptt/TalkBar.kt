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
// 대상은 **집합**이다. 코어에 팬아웃 API 가 없어 지금은 1개지만(MULTI_TALK_SUPPORTED) 칩·게이지·문구는
// 이미 집합 기준이라 상수 하나로 열린다.
package com.cims.ue.dispatch.ui.ptt

import com.cims.ue.dispatch.ui.Type
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
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
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@Composable
fun TalkBar(
    vm: PttChannelsViewModel,
    lockEnabled: Boolean,
    modifier: Modifier = Modifier,
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
        onFocus = vm::focus,
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
                label = when {
                    speaking && granted < targets.size -> "발언 $granted/${targets.size}"
                    speaking -> "발언 중"
                    requesting -> "요청 중"
                    locked -> "잠금"
                    else -> "PTT"
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
                        if (anyJoined) "채널 카드의 [발언 대상] 을 누르세요"
                        else "내 채널에서 [참여] 를 누르세요",
                        fontSize = Type.body)
                }
            } else {
                // 대상 칩 — 대상별 상태를 따로 본다(하나가 거부돼도 나머지는 살아 있다).
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.weight(1f)
                ) {
                    // 칩 = 그 채널로 포커스, × = 그 대상 하나만 빼기(데스크톱 칩 «클릭 = 포커스 · × = 체크 해제»). 여럿을 잡아 둔
                    //   채 하나만 빼려고 목록으로 돌아갈 필요가 없다.
                    targets.forEach { t ->
                        // 승인된 대상은 발언 색으로 채운다 — 여럿 중 어디로 나가고 있는지 칩에서 바로 읽힌다.
                        val on = t.granted
                        Surface(
                            color = if (on) p.live else p.paper, contentColor = if (on) p.onInk else p.ink,
                            shape = RoundedCornerShape(8.dp),
                            border = if (on) null else BorderStroke(1.dp, p.ink),
                            modifier = Modifier.height(36.dp).clickable { onFocus(t.card.id) },
                        ) {
                            Row(Modifier.padding(start = 12.dp, end = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                                Text(t.name + (if (t.stateText.isNotEmpty()) " · ${t.stateText}" else ""),
                                     fontSize = Type.body, maxLines = 1)
                                Icon(Icons.Filled.Close, contentDescription = "${t.name} 발언 대상에서 빼기",
                                    modifier = Modifier.padding(start = 4.dp).size(20.dp).clickable { onRemove(t.card.id) })
                            }
                        }
                    }
                }
                TextButton(onClick = onClear) { Text("모두 해제", fontSize = Type.body, color = p.ink) }
            }
        }
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
    width: androidx.compose.ui.unit.Dp = 150.dp,
    height: androidx.compose.ui.unit.Dp = 64.dp,
    compact: Boolean = false,
) {
    var pressed by remember { mutableStateOf(false) }
    val scheme = MaterialTheme.colorScheme
    val p = com.cims.ue.dispatch.ui.Tokens.palette

    // 대기 = 검정(시안), 누름·요청·대기열 = 진회색, **발언 = 초록**(손끝에서 «지금 나간다» 를 바로 알게), 비활성 = 띠 + 테두리.
    val bg = when {
        !enabled -> p.bar
        speaking -> p.live
        requesting || pressed || locked -> p.ink2
        else -> p.ink
    }
    val fg = when {
        !enabled -> p.faint
        else -> p.onInk
    }

    Surface(
        color = bg,
        contentColor = fg,
        shape = RoundedCornerShape(12.dp),
        border = if (enabled) null else BorderStroke(1.dp, p.line),
        modifier = Modifier
            .width(width)
            .height(height)
            .then(
                if (!enabled) Modifier
                else Modifier.pointerInput(Unit) {
                    detectTapGestures(onPress = {
                        pressed = true
                        onDown()
                        // 취소(제스처 무효화·코루틴 취소)에도 반드시 해제한다 — finally 가 유일한 보장이다.
                        try {
                            tryAwaitRelease()
                        } finally {
                            pressed = false
                            onUp()
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
                    if (!enabled) "대상 없음" else if (speaking) "말하세요" else "누르고 말하기",
                    fontSize = Type.meta)
            }
        }
    }
}
