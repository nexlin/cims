// 공용 조각 — 시안의 반복 요소를 한 벌로 (android_dispatch_tablet.md §6.3c)
//
// 시안은 같은 모양을 여러 화면에 되풀이한다 — 숫자 배지(검정 알약), 알약 버튼(36·32·44), 세그먼트([무전|통화]·[접속|편성]),
// 거르기 칩 줄, 구역 머리(48), 상태 점(9), 머리글자 아바타. 화면마다 따로 그리면 반경·굵기가 조금씩 어긋나 «같은 것인가» 가
// 흔들린다. 여기 한 벌만 두고 화면은 부르기만 한다. 색은 [Tokens] 에서만 읽는다.
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** 숫자 배지 — 검정 알약. 0 이하면 그리지 않는다. [inverted] = 검정 면 위에 놓일 때(흰 알약). */
@Composable
fun CountPill(n: Int, modifier: Modifier = Modifier, inverted: Boolean = false) {
    if (n <= 0) return
    val p = Tokens.palette
    Box(modifier.clip(RoundedCornerShape(8.dp)).background(if (inverted) p.onInk else p.ink)
            .padding(horizontal = 6.dp), contentAlignment = Alignment.Center) {
        Text(if (n > 99) "99+" else "$n", fontSize = Type.micro, fontWeight = FontWeight.Bold,
            color = if (inverted) p.ink else p.onInk)
    }
}

/** 라벨 모양 — 채운 면(옅은 회색)·테두리·강조(검정 면). */
enum class LabelStyle { FILL, OUTLINE, STRONG }

/** 한 낱말 라벨 — «채널 상세»·«그룹»·«1:1»·«청취 가능». 누를 수 없다. */
@Composable
fun Label(text: String, style: LabelStyle = LabelStyle.FILL, modifier: Modifier = Modifier,
          round: Boolean = false, color: Color? = null) {
    val p = Tokens.palette
    val shape = RoundedCornerShape(if (round) 10.dp else 4.dp)
    val (bg, fg, border) = when (style) {
        LabelStyle.FILL -> Triple(p.fill, color ?: p.ink, null)
        LabelStyle.OUTLINE -> Triple(Color.Transparent, color ?: p.ink2, BorderStroke(1.dp, color ?: p.line))
        LabelStyle.STRONG -> Triple(color ?: p.ink, p.onInk, null)
    }
    Surface(color = bg, contentColor = fg, shape = shape, border = border, modifier = modifier) {
        Text(text, Modifier.padding(horizontal = if (round) 8.dp else 6.dp, vertical = 1.dp),
            fontSize = Type.micro, maxLines = 1)
    }
}

/**
 * 알약 버튼 — 시안의 모든 누르는 글자(36 기본 · 32 작게 · 44 크게). [filled] = 검정 면(그 줄의 주 동작 하나),
 * 아니면 흰 면 + 테두리. [strongBorder] = 검정 테두리(주 동작 옆의 버금 동작).
 */
@Composable
fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    filled: Boolean = false,
    strongBorder: Boolean = false,
    height: Dp = 36.dp,
    enabled: Boolean = true,
    leading: ImageVector? = null,
    trailingNote: String = "",
    color: Color? = null,
) {
    val p = Tokens.palette
    val accent = color ?: p.ink
    val bg = when { !enabled -> p.bar; filled -> accent; else -> p.paper }
    val fg = when { !enabled -> p.faint; filled -> p.onInk; else -> color ?: p.ink }
    val border = when {
        filled && enabled -> null
        strongBorder || color != null -> BorderStroke(1.5.dp, if (enabled) accent else p.line)
        else -> BorderStroke(1.dp, p.line)
    }
    Surface(
        color = bg, contentColor = fg, shape = RoundedCornerShape(height / 2), border = border,
        modifier = modifier.height(height).clip(RoundedCornerShape(height / 2))
            .then(if (enabled) Modifier.clickable(onClick = onClick) else Modifier),
    ) {
        Row(Modifier.padding(horizontal = if (height >= 44.dp) 18.dp else 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center) {
            if (leading != null) {
                Icon(leading, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(6.dp))
            }
            Text(text, fontSize = if (height >= 44.dp) Type.strong else if (height <= 32.dp) Type.meta else Type.body,
                fontWeight = if (filled || strongBorder) FontWeight.Bold else FontWeight.Normal, maxLines = 1)
            if (trailingNote.isNotEmpty()) Text(" · $trailingNote", fontSize = Type.meta,
                color = if (filled) p.onInk else p.muted, maxLines = 1)
        }
    }
}

/**
 * 세그먼트 — 붙은 칸들 중 하나를 고른다([무전|통화]·[접속 7|편성 12]). 고른 칸 = 검정 면.
 *
 * @param itemWidth 칸 폭. null 이면 칸들이 폭을 나눠 갖는다(부모가 폭을 준다).
 * @param badges 칸마다의 숫자 배지(없으면 비운다).
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
) {
    val p = Tokens.palette
    Row(modifier.height(height).clip(RoundedCornerShape(height / 2))
            .border(1.5.dp, p.ink, RoundedCornerShape(height / 2))) {
        options.forEachIndexed { i, label ->
            val on = i == selected
            Row(
                (if (itemWidth != null) Modifier.width(itemWidth) else Modifier.weight(1f))
                    .fillMaxHeight().background(if (on) p.ink else p.paper)
                    .clickable { onSelect(i) },
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center,
            ) {
                Text(label, fontSize = if (height >= 36.dp) Type.strong else Type.body,
                    fontWeight = if (on) FontWeight.Bold else FontWeight.Normal,
                    color = if (on) p.onInk else p.ink, maxLines = 1)
                val n = badges.getOrNull(i) ?: 0
                if (n > 0) { Spacer(Modifier.width(6.dp)); CountPill(n, inverted = on) }
            }
        }
    }
}

/** 거르기 칩 하나 — 고르면 검정 면(12sp, 반경 6). */
@Composable
fun FilterPill(text: String, selected: Boolean, onClick: () -> Unit, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    Surface(
        color = if (selected) p.ink else p.paper, contentColor = if (selected) p.onInk else p.ink,
        shape = RoundedCornerShape(6.dp), border = if (selected) null else BorderStroke(1.dp, p.line),
        modifier = modifier.heightIn(min = 30.dp).clip(RoundedCornerShape(6.dp)).clickable(onClick = onClick),
    ) {
        Box(Modifier.padding(horizontal = 10.dp, vertical = 5.dp), contentAlignment = Alignment.Center) {
            Text(text, fontSize = Type.meta, maxLines = 1)
        }
    }
}

/** 구역 머리 — 높이 48, 제목 15 굵게 + 뒤에 붙는 것들(개수·라벨·버튼). */
@Composable
fun SectionHead(
    title: String,
    modifier: Modifier = Modifier,
    trailing: @Composable RowScope.() -> Unit = {},
) {
    Row(modifier.fillMaxWidth().height(48.dp).padding(start = 16.dp, end = 8.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title, fontSize = Type.title, fontWeight = FontWeight.Bold, maxLines = 1)
        trailing()
    }
}

/**
 * 상태 점(9dp) — 진행 중이면 진하게. 색은 호출자가 [dotColor] 로 고른다(긴급·임박·발언·진행·대기).
 */
@Composable
fun StatusDot(color: Color, modifier: Modifier = Modifier) {
    Box(modifier.size(9.dp).clip(CircleShape).background(color))
}

/**
 * 상태 점의 색 — 긴급 › 임박 › 발언 중 › 진행 중 › 대기. 시안의 세 단계(검정·진회색·옅은 회색)에 긴급·임박 색만 더한다.
 */
@Composable
fun dotColor(emergency: Boolean, peril: Boolean, speaking: Boolean, active: Boolean): Color {
    val p = Tokens.palette
    return when {
        emergency -> p.emergency
        peril -> p.peril
        speaking -> p.ink
        active -> p.muted
        else -> p.divider
    }
}

/** 머리글자 아바타 — 사람은 원, 그룹은 둥근 네모. [strong] = 검정 면(나). */
@Composable
fun Initial(text: String, modifier: Modifier = Modifier, size: Dp = 30.dp, square: Boolean = false, strong: Boolean = false) {
    val p = Tokens.palette
    Box(modifier.size(size).clip(if (square) RoundedCornerShape(8.dp) else CircleShape)
            .background(if (strong) p.ink else p.fill), contentAlignment = Alignment.Center) {
        Text(text.trim().take(1).ifEmpty { "?" }, fontSize = if (size >= 36.dp) Type.body else Type.meta,
            fontWeight = FontWeight.Bold, color = if (strong) p.onInk else p.ink)
    }
}

/** 세로 구분선 — 본문 칸 사이(1dp). */
@Composable
fun VDivider(modifier: Modifier = Modifier) {
    Box(modifier.width(1.dp).fillMaxHeight().background(Tokens.palette.divider))
}

/** 가로 구분선(1dp). [strong] = 검정 선(표 머리 아래). */
@Composable
fun HDivider(modifier: Modifier = Modifier, strong: Boolean = false, hair: Boolean = false) {
    val p = Tokens.palette
    Box(modifier.fillMaxWidth().height(1.dp).background(when { strong -> p.ink; hair -> p.hair; else -> p.divider }))
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
 * 고르는 칩 — Material3 `FilterChip` 에 시안의 색을 입힌 것(고르면 검정 면, 아니면 흰 면 + 테두리). 화면은 `FilterChip` 을
 * 직접 쓰지 않고 이것을 쓴다 — 고른 칩이 옅은 회색이면 «고른 것» 이 흐려진다. [colors] 를 주면 그 색이 이긴다(음소거 경고 등).
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
            containerColor = p.paper, labelColor = p.ink, iconColor = p.ink,
            selectedContainerColor = p.ink, selectedLabelColor = p.onInk,
            selectedLeadingIconColor = p.onInk, selectedTrailingIconColor = p.onInk),
        border = androidx.compose.material3.FilterChipDefaults.filterChipBorder(
            enabled = enabled, selected = selected, borderColor = p.line, selectedBorderColor = p.ink,
            borderWidth = 1.dp, selectedBorderWidth = 1.dp),
    )
}
