// 공용 조각 — 반복 요소를 한 벌로 (android_dispatch_tablet.md §6.3c)
//
// 같은 모양이 여러 화면에 되풀이된다 — 수 배지(남색 둥근 8), 알약 버튼(30·32·36·44), 세그먼트([무전|통화]·[접속|편성]),
// 거르기 칩 줄, 라벨(19, 모서리 4), 구역 머리(48), 상태 점(9), 아바타. 화면마다 따로 그리면 반경·굵기가 조금씩 어긋나
// «같은 것인가» 가 흔들린다. 여기 한 벌만 두고 화면은 부르기만 한다. 색은 [Tokens] 에서만 읽는다.
//
// 낱말은 Windows 관제 앱의 스타일 이름과 같다(dispatch_desktop_ui.md §3.2 «시각 언어» — `Pill.*`·`Tag.*`·`FilterChip`·
// `Count`·`Avatar`·`TargetCheck`·`ModeSeg`): 채운 주 행동 = 남색, 통화 행동 = 녹색, 선택 = 연한 남색 면 + 남색 외곽선·글자.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** 비활성의 흐림 — 버튼·칩·세그먼트 공통(Windows `IsEnabled=False → Opacity 0.4`). */
const val DisabledAlpha = 0.4f

/** 면·글자·테두리 한 벌 — 라벨·알약 버튼이 색조에서 얻는다. */
@Immutable
data class Swatch(val bg: Color, val fg: Color, val border: Color? = null)

/** 수 배지 — 남색 둥근 8. 0 이하면 그리지 않는다. [inverted] = 남색 면 위에 놓일 때(흰 알약 + 남색 글자). */
@Composable
fun CountPill(n: Int, modifier: Modifier = Modifier, inverted: Boolean = false) {
    if (n <= 0) return
    val p = Tokens.palette
    Box(modifier.heightIn(min = 17.dp).clip(RoundedCornerShape(8.dp)).background(if (inverted) p.onPrimary else p.primary)
            .padding(horizontal = 6.dp), contentAlignment = Alignment.Center) {
        Text(if (n > 99) "99+" else "$n", fontSize = Type.micro, fontWeight = FontWeight.Bold,
            color = if (inverted) p.primary else p.onPrimary)
    }
}

/**
 * 라벨의 색조 — Windows `Tag.*` 와 같은 낱말.
 *
 * 기본([FILL]) = 연한 회색 면(개별·애드혹·SDS) · [INK] = 연한 남색(일제·그룹) · [EMG] = 빨강 채움(긴급) · [PERIL] = 주황 채움(임박) ·
 * 나머지는 연한 상태색 면 + 그 색의 진한 글자(통화 중·부재·문자·전달·감청·대표). [STRONG] = 남색 채움(«그룹 전원» 처럼 그 줄의
 * 머리 라벨), [OUTLINE] = 면 없이 테두리만(«1:1»).
 */
enum class LabelStyle { FILL, OUTLINE, STRONG, INK, EMG, PERIL, RED, MON, PILOT, TEAL, LISTEN, TALK, HELD }

@Composable
internal fun LabelStyle.swatch(): Swatch {
    val p = Tokens.palette
    return when (this) {
        LabelStyle.FILL -> Swatch(p.fill, p.ink2, p.divider)
        LabelStyle.OUTLINE -> Swatch(Color.Transparent, p.ink2, p.faint)
        LabelStyle.STRONG -> Swatch(p.primary, p.onPrimary, p.primary)
        LabelStyle.INK -> Swatch(p.primarySoft, p.primaryInk, p.primaryEdge)
        LabelStyle.EMG -> Swatch(p.emgFill, p.onAccent, p.emgFill)
        LabelStyle.PERIL -> Swatch(p.peril, p.onPeril, p.peril)
        LabelStyle.RED -> Swatch(p.emgSoft, p.emg, p.emgEdge)
        LabelStyle.MON -> Swatch(p.monSoft, p.monInk, p.monEdge)
        LabelStyle.PILOT -> Swatch(p.ringSoft, p.ringInk, p.ringEdge)
        LabelStyle.TEAL -> Swatch(p.listenSoft, p.listenInk, p.listenSoft)
        LabelStyle.LISTEN -> Swatch(p.listenSoft, p.listenInk, p.listen)
        LabelStyle.TALK -> Swatch(p.talkSoft, p.talkInk, p.talkEdge)
        LabelStyle.HELD -> Swatch(p.heldSoft, p.heldInk, p.heldSoft)
    }
}

/** 한 낱말 라벨(19, 모서리 4) — «채널 상세»·«그룹»·«1:1»·«긴급»·«청취 가능». 누를 수 없다. [round] = 알약 모양(모서리 10). */
@Composable
fun Label(text: String, style: LabelStyle = LabelStyle.FILL, modifier: Modifier = Modifier, round: Boolean = false,
          bold: Boolean = true) {
    val s = style.swatch()
    val shape = RoundedCornerShape(if (round || style == LabelStyle.LISTEN) 10.dp else 4.dp)
    Surface(color = s.bg, contentColor = s.fg, shape = shape, border = s.border?.let { BorderStroke(1.dp, it) },
        modifier = modifier) {
        Text(text, Modifier.padding(horizontal = if (round || style == LabelStyle.LISTEN) 8.dp else 5.dp, vertical = 1.dp),
            fontSize = Type.micro, fontWeight = if (bold) FontWeight.Bold else FontWeight.Normal, maxLines = 1)
    }
}

/**
 * 알약 버튼의 종류 — Windows `Pill.*`.
 *
 * [SOFT] 연한 외곽선(그 줄의 버금 동작) · [LINE] 진한 외곽선 1.5 + 굵은 글자 · [INK] 남색 채움(그 줄의 주 행동 하나) ·
 * [CALL] 녹색 채움(응답·발신·당겨받기) · [RED] 빨강 외곽선·글자([긴급]·[삭제]) · [RED_FILL] 빨강 채움([종료]·음소거 중) ·
 * [TALK] 연한 녹색(발언 중) · [LISTEN] 연한 청록 · [LISTEN_LINE] 청록 외곽선([청취]) · [LISTEN_FILL] 청록 채움([청취 중]·[보기]) ·
 * [PERIL_FILL] 주황 채움(임박) · [MON_FILL] 보라 채움(경보) · [ON_PERIL] 주황 면 위의 먹 채움.
 */
enum class Pill {
    SOFT, LINE, INK, CALL, RED, RED_FILL, TALK, LISTEN, LISTEN_LINE, LISTEN_FILL, PERIL_FILL, MON_FILL, ON_PERIL,
    /** 배너 위의 테두리 알약 — 면은 비우고 글자·테두리가 그 배너의 글자색을 따른다(긴급·임박·경보 배너의 [해제]·[닫기]). */
    EMG_LINE, ON_PERIL_LINE, MON_LINE,
}

@Immutable
private data class PillLook(val bg: Color, val fg: Color, val border: BorderStroke?, val bold: Boolean)

@Composable
private fun Pill.look(): PillLook {
    val p = Tokens.palette
    return when (this) {
        Pill.SOFT -> PillLook(p.paper, p.ink, BorderStroke(1.dp, p.line), false)
        Pill.LINE -> PillLook(p.paper, p.ink, BorderStroke(1.5.dp, p.edge), true)
        Pill.INK -> PillLook(p.primary, p.onPrimary, null, true)
        Pill.CALL -> PillLook(p.talkFill, p.onAccent, null, true)
        Pill.RED -> PillLook(p.paper, p.emg, BorderStroke(1.5.dp, p.emg), true)
        Pill.RED_FILL -> PillLook(p.emgFill, p.onAccent, null, true)
        Pill.TALK -> PillLook(p.talkSoft, p.talkInk, BorderStroke(1.5.dp, p.talkInk), true)
        Pill.LISTEN -> PillLook(p.listenSoft, p.listenInk, BorderStroke(1.5.dp, p.listen), true)
        Pill.LISTEN_LINE -> PillLook(p.paper, p.listenInk, BorderStroke(1.5.dp, p.listen), true)
        Pill.LISTEN_FILL -> PillLook(p.listenFill, p.onAccent, null, true)
        Pill.PERIL_FILL -> PillLook(p.peril, p.onPeril, null, true)
        Pill.MON_FILL -> PillLook(p.monFill, p.onAccent, null, true)
        Pill.ON_PERIL -> PillLook(p.onPeril, p.peril, null, true)
        Pill.EMG_LINE -> PillLook(Color.Transparent, p.emgInk, BorderStroke(1.5.dp, p.emgInk), true)
        Pill.ON_PERIL_LINE -> PillLook(Color.Transparent, p.onPeril, BorderStroke(1.5.dp, p.onPeril), true)
        Pill.MON_LINE -> PillLook(Color.Transparent, p.monInk, BorderStroke(1.5.dp, p.monInk), true)
    }
}

/**
 * 알약 버튼 — 모든 누르는 글자(30 조작 줄 · 32/36 기본 · 44 바닥 행동). 모서리 = 높이의 절반. 비활성 = 흐리게.
 */
@Composable
fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    kind: Pill = Pill.SOFT,
    height: Dp = 36.dp,
    enabled: Boolean = true,
    leading: ImageVector? = null,
    trailingNote: String = "",
) {
    val look = kind.look()
    val shape = RoundedCornerShape(height / 2)
    Surface(
        color = look.bg, contentColor = look.fg, shape = shape, border = look.border,
        modifier = modifier.height(height).alpha(if (enabled) 1f else DisabledAlpha).clip(shape)
            .then(if (enabled) Modifier.clickable(onClick = onClick) else Modifier),
    ) {
        Row(Modifier.padding(horizontal = if (height >= 44.dp) 20.dp else 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
            if (leading != null) {
                Icon(leading, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(6.dp))
            }
            Text(text, fontSize = if (height >= 44.dp) Type.strong else if (height <= 32.dp) Type.meta else Type.body,
                fontWeight = if (look.bold) FontWeight.Bold else FontWeight.Normal, maxLines = 1)
            if (trailingNote.isNotEmpty()) Text(" · $trailingNote", fontSize = Type.meta, maxLines = 1,
                modifier = Modifier.alpha(0.8f))
        }
    }
}

/**
 * 네모 버튼의 종류 — Windows `Rect.*`. [SOFT] 연한 외곽선(통화 카드 조작) · [INK] 남색 채움 · [CALL] 녹색 채움([발신]·[응답]) ·
 * [RED] 빨강 채움([종료]·[거절]) · [ON] 켜진 토글(연한 남색 면 + 남색 외곽선 — [DTMF]·[전달] 이 펴져 있다) ·
 * [RED_LINE] 빨강 외곽선([청취 종료]) · [LISTEN_LINE] 청록 외곽선([청취]).
 */
enum class Rect { SOFT, INK, CALL, RED, ON, RED_LINE, LISTEN_LINE }

/**
 * 네모 버튼(모서리 8) — 통화 축의 조작. 알약([PillButton])과 가르는 것은 뜻이다: 알약 = 무전·패널의 행동, 네모 = 통화 카드의 조작
 * (데스크톱 «시각 언어» 와 같다).
 */
@Composable
fun RectButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    kind: Rect = Rect.SOFT,
    height: Dp = 36.dp,
    enabled: Boolean = true,
    bold: Boolean = false,
) {
    val p = Tokens.palette
    val (bg, fg, border) = when (kind) {
        Rect.SOFT -> Triple(p.paper, p.ink, p.line)
        Rect.INK -> Triple(p.primary, p.onPrimary, null)
        Rect.CALL -> Triple(p.talkFill, p.onAccent, null)
        Rect.RED -> Triple(p.emgFill, p.onAccent, null)
        Rect.ON -> Triple(p.primarySoft, p.primaryInk, p.primaryLine)
        Rect.RED_LINE -> Triple(p.paper, p.emg, p.emg)
        Rect.LISTEN_LINE -> Triple(p.paper, p.listenInk, p.listen)
    }
    val filled = border == null
    val shape = RoundedCornerShape(8.dp)
    Surface(
        color = bg, contentColor = fg, shape = shape, border = border?.let { BorderStroke(1.dp, it) },
        modifier = modifier.height(height).alpha(if (enabled) 1f else DisabledAlpha).clip(shape)
            .then(if (enabled) Modifier.clickable(onClick = onClick) else Modifier),
    ) {
        Box(Modifier.padding(horizontal = if (filled) 16.dp else 12.dp), contentAlignment = Alignment.Center) {
            Text(text, fontSize = if (height <= 32.dp) Type.meta else Type.body, maxLines = 1,
                fontWeight = if (filled || bold || kind == Rect.RED_LINE) FontWeight.Bold else FontWeight.Normal)
        }
    }
}

/**
 * 세그먼트 — 붙은 칸들 중 하나를 고른다.
 *
 * [strong] = 모드 세그먼트([무전|통화] — Windows `ModeSeg`): 남색 외곽선 한 겹(안쪽 여백 2) 안의 둥근 칸, 고른 칸 = 남색 채움.
 * 아니면 작은 세그먼트([접속 7|편성 12] — Windows `Seg`): 옅은 외곽선, 고른 칸 = 연한 남색 면 + 남색 굵은 글자.
 *
 * @param itemWidth 칸 폭. null 이면 칸들이 폭을 나눠 갖는다(부모가 폭을 준다).
 * @param badges 칸마다의 수 배지(없으면 비운다).
 */
@Composable
fun Segmented(
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    height: Dp = 36.dp,
    itemWidth: Dp? = null,
    badges: List<Int> = emptyList(),
    strong: Boolean = true,
) {
    val p = Tokens.palette
    val outer = RoundedCornerShape(if (strong) 9.dp else 8.dp)
    Row(modifier.height(height).clip(outer).background(p.paper)
            .border(if (strong) 1.5.dp else 1.dp, if (strong) p.primary else p.line, outer)
            .padding(if (strong) 3.5.dp else 1.dp)) {
        options.forEachIndexed { i, label ->
            val on = i == selected
            val bg = when { !on -> Color.Transparent; strong -> p.primary; else -> p.primarySoft }
            val fg = when { !on -> if (strong) p.ink2 else p.muted; strong -> p.onPrimary; else -> p.primaryInk }
            Row(
                (if (itemWidth != null) Modifier.width(itemWidth) else Modifier.weight(1f))
                    .fillMaxHeight().clip(RoundedCornerShape(6.dp)).background(bg)
                    .clickable { onSelect(i) },
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center,
            ) {
                Text(label, fontSize = if (height >= 36.dp) Type.strong else Type.body,
                    fontWeight = if (on || strong) FontWeight.Bold else FontWeight.Normal, color = fg, maxLines = 1)
                val n = badges.getOrNull(i) ?: 0
                if (n > 0) { Spacer(Modifier.width(6.dp)); CountPill(n, inverted = on && strong) }
            }
        }
    }
}

/** 거르기 칩 하나(모서리 6) — 켜짐 = 연한 남색 면 + 남색 외곽선 + 남색 굵은 글자(Windows `FilterChip`). */
@Composable
fun FilterPill(text: String, selected: Boolean, onClick: () -> Unit, modifier: Modifier = Modifier,
               dot: Color? = null) {
    val p = Tokens.palette
    Surface(
        color = if (selected) p.primarySoft else p.paper, contentColor = if (selected) p.primaryInk else p.ink2,
        shape = RoundedCornerShape(6.dp), border = BorderStroke(1.dp, if (selected) p.primaryLine else p.line),
        modifier = modifier.heightIn(min = 30.dp).clip(RoundedCornerShape(6.dp)).clickable(onClick = onClick),
    ) {
        Row(Modifier.padding(horizontal = 10.dp, vertical = 5.dp), verticalAlignment = Alignment.CenterVertically) {
            // 종류 색 점 — 표의 종류 라벨과 같은 색(범례).
            if (dot != null) { Box(Modifier.size(7.dp).clip(CircleShape).background(dot)); Spacer(Modifier.width(5.dp)) }
            Text(text, fontSize = Type.meta, fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal, maxLines = 1)
        }
    }
}

/**
 * 구역 머리 — 높이 48, 제목 15 굵게 + 뒤에 붙는 것들(개수·라벨·버튼).
 *
 * @param end 오른쪽 여백 — 끝에 아이콘 단추(40)가 서면 8, 글자로 끝나면 16.
 */
@Composable
fun SectionHead(
    title: String,
    modifier: Modifier = Modifier,
    end: Dp = 8.dp,
    trailing: @Composable RowScope.() -> Unit = {},
) {
    Row(modifier.fillMaxWidth().height(48.dp).padding(start = 16.dp, end = end),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title, fontSize = Type.title, fontWeight = FontWeight.Bold, maxLines = 1)
        trailing()
    }
}

/** 상태 점(9dp). 색은 호출자가 [dotColor] 로 고른다(긴급·임박·참여·진행·대기). */
@Composable
fun StatusDot(color: Color, modifier: Modifier = Modifier) {
    Box(modifier.size(9.dp).clip(CircleShape).background(color))
}

/**
 * 상태 점의 색 — 긴급(빨강) › 임박(주황) › 청취 중(청록) › 참여 중(남색) › 진행 중(회색) › 대기(옅은 선). Windows 채널 카드·타 채널
 * 행의 점과 같은 서열이다(dispatch_desktop_ui.md §4).
 */
@Composable
fun dotColor(emergency: Boolean, peril: Boolean, joined: Boolean, active: Boolean, listening: Boolean = false): Color {
    val p = Tokens.palette
    return when {
        emergency -> p.emg
        peril -> p.peril
        listening -> p.listen
        joined -> p.primaryLine
        active -> p.muted
        else -> p.divider
    }
}

/**
 * 아바타 — 사람은 원, 그룹은 둥근 네모(모서리 8). 색은 **이름 해시**로 8색 중 하나([avatarHueOf] — 같은 사람은 어느 목록에서나
 * 같은 색이고 Windows 관제 앱과도 같다). [strong] = 남색 채움(나).
 */
@Composable
fun Initial(text: String, modifier: Modifier = Modifier, size: Dp = 30.dp, square: Boolean = false, strong: Boolean = false) {
    val p = Tokens.palette
    val hue = p.avatars[avatarHueOf(text)]
    Box(modifier.size(size).clip(if (square) RoundedCornerShape(8.dp) else CircleShape)
            .background(if (strong) p.primary else hue.bg), contentAlignment = Alignment.Center) {
        Text(initialOf(text), fontSize = if (size >= 36.dp) Type.body else Type.meta,
            fontWeight = FontWeight.Bold, color = if (strong) p.onPrimary else hue.fg)
    }
}

/** 입력칸 모서리 — 데스크톱 입력칸과 같은 6. */
val FieldShape6 = RoundedCornerShape(6.dp)

/**
 * 입력칸의 색 — 옅은 면(BgSoft) + 진한 테두리(BorderStrong), 포커스 = 남색 선(데스크톱 `Controls.xaml` 입력칸). Material 기본
 * (모서리 4·투명 면)을 그대로 쓰지 않는다 — 로그인·설정의 `OutlinedTextField` 가 [FieldShape6] 과 함께 쓴다.
 */
@Composable
fun cimsFieldColors(): androidx.compose.material3.TextFieldColors {
    val p = Tokens.palette
    return androidx.compose.material3.OutlinedTextFieldDefaults.colors(
        focusedBorderColor = p.primaryLine, unfocusedBorderColor = p.edge, disabledBorderColor = p.line,
        focusedContainerColor = p.canvas, unfocusedContainerColor = p.canvas, disabledContainerColor = p.bar,
        focusedLabelColor = p.primaryInk, unfocusedLabelColor = p.muted, disabledLabelColor = p.faint,
        focusedTextColor = p.ink, unfocusedTextColor = p.ink, disabledTextColor = p.muted,
        cursorColor = p.primaryLine)
}

/** 아바타 머리글자 — 이름의 첫 글자. 이름 없이 번호만 있으면 «#»(데스크톱 `InitialOf` — «+»·«0» 이 머리글자로 서지 않는다). */
internal fun initialOf(label: String): String {
    val t = label.trim()
    return if (t.isEmpty()) "?" else if (t[0].isDigit() || t[0] == '+') "#" else t.take(1)
}

/** 세로 구분선 — 본문 칸 사이(1dp). */
@Composable
fun VDivider(modifier: Modifier = Modifier) {
    Box(modifier.width(1.dp).fillMaxHeight().background(Tokens.palette.divider))
}

/** 가로 구분선(1dp). [strong] = 진한 선(표 머리 아래 — Edge). */
@Composable
fun HDivider(modifier: Modifier = Modifier, strong: Boolean = false, hair: Boolean = false) {
    val p = Tokens.palette
    Box(modifier.fillMaxWidth().height(1.dp).background(when { strong -> p.edge; hair -> p.hair; else -> p.divider }))
}

/** 두 줄 글자(제목·보조) — 행마다 되풀이되는 모양. */
@Composable
fun TwoLines(title: String, sub: String, modifier: Modifier = Modifier, titleWeight: FontWeight = FontWeight.Bold,
             titleSize: androidx.compose.ui.unit.TextUnit = Type.strong) {
    val p = Tokens.palette
    androidx.compose.foundation.layout.Column(modifier) {
        Text(title, fontSize = titleSize, fontWeight = titleWeight, maxLines = 1, overflow = TextOverflow.Ellipsis)
        if (sub.isNotEmpty()) Text(sub, fontSize = Type.meta, color = p.muted, maxLines = 1,
            overflow = TextOverflow.Ellipsis, lineHeight = 16.sp)
    }
}

/**
 * 고르는 칩 — Material3 `FilterChip` 에 앱의 색을 입힌 것(고르면 연한 남색 면 + 남색 외곽선·글자). 화면은 `FilterChip` 을 직접
 * 쓰지 않고 이것을 쓴다. [colors] 를 주면 그 색이 이긴다(음소거 경고 등).
 */
@Composable
fun CimsFilterChip(
    selected: Boolean,
    onClick: () -> Unit,
    label: @Composable () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    leadingIcon: (@Composable () -> Unit)? = null,
    trailingIcon: (@Composable () -> Unit)? = null,
    colors: androidx.compose.material3.SelectableChipColors? = null,
) {
    val p = Tokens.palette
    androidx.compose.material3.FilterChip(
        selected = selected, onClick = onClick, label = label, modifier = modifier, enabled = enabled,
        leadingIcon = leadingIcon, trailingIcon = trailingIcon,
        shape = RoundedCornerShape(6.dp),
        colors = colors ?: androidx.compose.material3.FilterChipDefaults.filterChipColors(
            containerColor = p.paper, labelColor = p.ink2, iconColor = p.ink2,
            selectedContainerColor = p.primarySoft, selectedLabelColor = p.primaryInk,
            selectedLeadingIconColor = p.primaryInk, selectedTrailingIconColor = p.primaryInk),
        border = androidx.compose.material3.FilterChipDefaults.filterChipBorder(
            enabled = enabled, selected = selected, borderColor = p.line, selectedBorderColor = p.primaryLine,
            borderWidth = 1.dp, selectedBorderWidth = 1.dp),
    )
}

/** 빨강 채움 칩 색 — 음소거 중처럼 «켜져 있으면 위험한» 토글(Windows `RoundBtn` 의 음소거 중). */
@Composable
fun warnChipColors(): androidx.compose.material3.SelectableChipColors {
    val p = Tokens.palette
    return androidx.compose.material3.FilterChipDefaults.filterChipColors(
        containerColor = p.paper, labelColor = p.ink2, iconColor = p.ink2,
        selectedContainerColor = p.emgFill, selectedLabelColor = p.onAccent,
        selectedLeadingIconColor = p.onAccent, selectedTrailingIconColor = p.onAccent)
}

/**
 * 가는 슬라이더 — 선(4) + 둥근 손잡이(18). Material 슬라이더의 굵은 막대는 조밀한 줄(감청 음량·설정)에서 버튼처럼 읽혀
 * 옆의 조작과 헷갈린다. 값의 범위·단계는 Material 것 그대로다.
 */
@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
fun CimsSlider(
    value: Float,
    onValueChange: (Float) -> Unit,
    modifier: Modifier = Modifier,
    valueRange: ClosedFloatingPointRange<Float> = 0f..1f,
    steps: Int = 0,
    tint: Color = Tokens.palette.primary,
) {
    val p = Tokens.palette
    androidx.compose.material3.Slider(
        value = value, onValueChange = onValueChange, modifier = modifier.height(32.dp),
        valueRange = valueRange, steps = steps,
        thumb = { Box(Modifier.size(18.dp).background(tint, CircleShape)) },
        track = { st ->
            val span = (st.valueRange.endInclusive - st.valueRange.start).takeIf { it > 0f } ?: 1f
            val frac = ((st.value - st.valueRange.start) / span).coerceIn(0f, 1f)
            Box(Modifier.fillMaxWidth().height(4.dp).clip(RoundedCornerShape(2.dp)).background(p.line)) {
                Box(Modifier.fillMaxWidth(frac).height(4.dp).background(tint))
            }
        })
}
