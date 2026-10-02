// 녹취 재생 바 · 영상 칸 (android_dispatch_tablet.md §6.11, dispatch_desktop_ui.md §4.6 «녹취 재생 바»)
//
// 통화·무전 상세 패널 맨 아래에 고정되는 공용 조각이다. 막대 하나 = 녹취 전체의 벽시계 구간 — 재생하면 차오르며
// `지금 / 전체` 와 그 순간의 실제 시각을 보인다. **막대를 누르면 그 지점부터** 튼다(짚은 채 끌면 그 지점의 시각·말한 사람이
// 말풍선으로 따라오고, 떼면 거기서 튼다 — 데스크톱의 마우스 올림에 해당한다).
// 무전은 막대 위에 **발언 막대**(화자 레인 색, 겹치는 동시 발언은 두 번째 줄)가 놓인다.
package com.cims.ue.dispatch.ui.history

import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.zIndex
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.HDivider
import com.cims.ue.dispatch.ui.Label
import com.cims.ue.dispatch.ui.LabelStyle
import com.cims.ue.dispatch.ui.Rect
import com.cims.ue.dispatch.ui.RectButton
import com.cims.ue.dispatch.ui.Segmented
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type

/** 영상 칸의 폭 — 상세 패널 오른쪽에 열리고, 그만큼 상세 패널이 준다. */
internal val VideoPaneWidth = 300.dp

/**
 * 녹취 재생 바 — 상태 줄(· 건너뛰기 · 속도 · [다시 변환]) → 막대 → 조작 줄.
 *
 * 조작 = [재생/일시정지] · 통화 [−10초]·[+10초] / 무전 [이전 발언]·[다음 발언] · 속도 [1× 1.5× 2×] · [다시 변환](변환 실패 표식을
 * 지우고 그 자리에서) · 무전은 조작 줄 끝에 «지금 <이름> · 발언 #n»/«말 없는 구간», **말 없는 구간 건너뛰기**(기본 켬 — 끄면
 * 세그먼트 사이 빈 시간을 시계로 흘려 다음 세그먼트에서 이어 튼다).
 */
@Composable
internal fun PlayerBarView(player: PlayerUi, ptt: Boolean, act: HistoryActions, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    val bar = player.bar
    Column(modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("녹취", fontSize = Type.body, fontWeight = FontWeight.Bold, color = p.ink)
            Text(player.status, Modifier.weight(1f), fontSize = Type.meta, color = p.muted, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
            if (player.has) {
                if (ptt) FilterPill("말 없는 구간 건너뛰기", player.skipGaps, onClick = { act.setSkipGaps(!player.skipGaps) })
                Segmented(options = listOf("1×", "1.5×", "2×"), selected = player.speedIndex, onSelect = act.setSpeed,
                    height = 28.dp, itemWidth = 44.dp, strong = false)
                RectButton("다시 변환", act.retry, height = 28.dp, enabled = player.canPlay)
            }
        }
        if (bar == null || !player.has) return@Column
        Spacer(Modifier.height(if (ptt) 8.dp else 4.dp))
        SeekBar(player, bar, ptt, act.seek)
        if (ptt) Row(Modifier.fillMaxWidth().padding(top = 2.dp), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(hhmmss(bar.t0), fontSize = Type.micro, color = p.muted)
            Text(hhmmss(bar.endMs), fontSize = Type.micro, color = p.muted)
        }
        Spacer(Modifier.height(6.dp))
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            if (ptt) RectButton("이전 발언", act.prevTurn, height = 32.dp) else RectButton("−10초", act.back10, height = 32.dp)
            RectButton(if (player.playing) "일시정지" else "▶ 재생", act.togglePlay, Modifier.widthIn(min = 92.dp),
                kind = Rect.INK, height = 32.dp, enabled = player.canPlay || player.playing)
            if (ptt) RectButton("다음 발언", act.nextTurn, height = 32.dp) else RectButton("+10초", act.fwd10, height = 32.dp)
            Text(buildAnnotatedString {
                withStyle(SpanStyle(fontWeight = FontWeight.SemiBold, color = p.ink)) { append(player.posText) }
                withStyle(SpanStyle(color = p.faint)) { append(" / ${player.lenText}") }
            }, Modifier.padding(start = 4.dp), fontSize = Type.body, maxLines = 1)
            if (ptt) Text(player.wallText, Modifier.padding(start = 4.dp), fontSize = Type.meta, color = p.muted, maxLines = 1)
            Spacer(Modifier.weight(1f))
            if (ptt) {
                // 지금 누가 말하는가 — 말하는 동안은 발언 색.
                val shape = RoundedCornerShape(6.dp)
                Box(Modifier.clip(shape).background(if (player.speaking) p.talkSoft else p.fill).padding(horizontal = 10.dp, vertical = 4.dp)) {
                    Text(player.nowText, fontSize = Type.meta, fontWeight = FontWeight.Bold,
                        color = if (player.speaking) p.talkInk else p.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            } else Text("실제 시각 ${player.wallText}", fontSize = Type.meta, color = p.muted, maxLines = 1)
        }
    }
}

/**
 * 막대 — 통화는 가는 선 + 둥근 손잡이, 무전은 발언 막대 + 재생 헤드(세로 선).
 *
 * 누르는 일은 한 가지 제스처다: 짚으면 그 지점의 말풍선이 뜨고(끌면 따라온다) 떼면 그 지점부터 튼다. 제스처가 가로채이면
 * (취소) 옮기지 않는다.
 */
@Composable
private fun SeekBar(player: PlayerUi, bar: PlayerBar, ptt: Boolean, onSeek: (Float) -> Unit) {
    val p = Tokens.palette
    var hover by remember { mutableStateOf<Float?>(null) }
    val seek by rememberUpdatedState(onSeek)
    val height: Dp = if (ptt) 46.dp else 28.dp
    val shape = RoundedCornerShape(6.dp)
    BoxWithConstraints(Modifier.fillMaxWidth().height(height)) {
        val width = maxWidth
        val frame = if (ptt) Modifier.clip(shape).background(p.canvas).border(1.dp, p.hair, shape) else Modifier
        Canvas(Modifier.fillMaxSize().then(frame).pointerInput(Unit) {
            awaitEachGesture {
                val down = awaitFirstDown()
                down.consume()
                fun ratioOf(x: Float) = (x / size.width).coerceIn(0f, 1f)
                var at = ratioOf(down.position.x)
                var released = false
                hover = at
                try {
                    while (true) {
                        val change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: break
                        at = ratioOf(change.position.x)
                        change.consume()
                        if (!change.pressed) { released = true; break }
                        hover = at
                    }
                } finally { hover = null }
                if (released) seek(at)
            }
        }) {
            val head = player.headRatio * size.width
            if (ptt) {
                val h = 15.dp.toPx()
                bar.blocks.forEach { b ->
                    drawRoundRect(p.laneColor(b.lane).copy(alpha = 0.8f),
                        topLeft = Offset(b.left * size.width, (if (b.row == 0) 5.dp else 24.dp).toPx()),
                        size = Size(maxOf(b.width * size.width, 3.dp.toPx()), h), cornerRadius = CornerRadius(3.dp.toPx()))
                }
                drawRect(p.ink, topLeft = Offset(head - 1.dp.toPx(), 0f), size = Size(2.dp.toPx(), size.height))
            } else {
                val y = size.height / 2
                val t = 4.dp.toPx()
                drawRoundRect(p.divider, topLeft = Offset(0f, y - t / 2), size = Size(size.width, t), cornerRadius = CornerRadius(t / 2))
                drawRoundRect(p.primary, topLeft = Offset(0f, y - t / 2), size = Size(head, t), cornerRadius = CornerRadius(t / 2))
                drawCircle(p.paper, radius = 8.dp.toPx(), center = Offset(head, y))
                drawCircle(p.primary, radius = 7.dp.toPx(), center = Offset(head, y), style = Stroke(width = 3.dp.toPx()))
            }
        }
        // 짚은 지점의 말풍선 — 지금 위치 · 실제 시각(무전은 + 그때 말한 사람)
        hover?.let { r ->
            val bubble = 190.dp
            Box(Modifier.zIndex(1f).offset(x = (width * r - bubble / 2).coerceIn(0.dp, (width - bubble).coerceAtLeast(0.dp)), y = (-28).dp)
                    .width(bubble).clip(RoundedCornerShape(5.dp)).background(p.primary).padding(horizontal = 8.dp, vertical = 3.dp),
                contentAlignment = Alignment.Center) {
                Text(bar.hoverText(r), fontSize = Type.micro, color = p.onPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

/**
 * 영상 칸 — 세그먼트에 영상이 있으면 그 세그먼트가 열려 있는 동안 상세 패널 오른쪽에 열려 그 MP4 의 영상을 그린다(비율 유지).
 * 재생 바가 그대로 조작한다(누른 곳·속도·일시정지 — 일시정지는 칸을 닫지 않는다). 영상 없는 세그먼트·말 없는 구간으로 넘어가거나
 * [정지] 하면 닫힌다.
 *
 * 그리는 면은 `TextureView` 다 — Compose 배치 안에서 잘리고 겹치는 것이 다른 조각과 같다. 면이 생기면 [HistoryActions.surface]
 * 로 재생기에 붙이고, 사라지면 뗀다.
 */
@Composable
internal fun VideoPane(player: PlayerUi, act: HistoryActions, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    val attach by rememberUpdatedState(act.surface)
    Column(modifier.background(p.paper)) {
        Row(Modifier.fillMaxWidth().background(p.bar).padding(horizontal = 14.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("녹취 영상", fontSize = Type.strong, fontWeight = FontWeight.Bold, color = p.ink, maxLines = 1)
            if (player.playing) Label("재생 중", LabelStyle.TEAL)
            Spacer(Modifier.weight(1f))
            RectButton("정지", act.stop, height = 28.dp)
        }
        HDivider()
        // 영상 면 — 남는 자리 안에서 영상의 비율을 지킨다(위아래 또는 좌우가 남는다). 바탕은 데스크톱 영상 칸과 같은 먹색.
        BoxWithConstraints(Modifier.weight(1f).fillMaxWidth().padding(12.dp).clip(RoundedCornerShape(8.dp))
                .background(p.ink), contentAlignment = Alignment.Center) {
            val aspect = player.videoAspect.coerceIn(0.2f, 5f)
            val wide = maxWidth / maxHeight < aspect
            val fit = if (wide) Modifier.fillMaxWidth().aspectRatio(aspect)
                      else Modifier.fillMaxHeight().aspectRatio(aspect, matchHeightConstraintsFirst = true)
            if (LocalInspectionMode.current) Box(fit.border(1.dp, p.faint), contentAlignment = Alignment.Center) {
                Text("영상", fontSize = Type.body, color = p.faint)
            } else AndroidView(modifier = fit, factory = { ctx ->
                TextureView(ctx).apply {
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        private var surface: Surface? = null
                        override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                            surface = Surface(st).also { attach(it) }
                        }
                        override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) = Unit
                        override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
                            attach(null)
                            surface?.release()
                            surface = null
                            return true
                        }
                        override fun onSurfaceTextureUpdated(st: SurfaceTexture) = Unit
                    }
                }
            })
        }
        Text("${player.posText} / ${player.lenText} · ${player.wallText}",
            Modifier.padding(start = 14.dp, end = 14.dp, bottom = 10.dp), fontSize = Type.meta, color = p.muted, maxLines = 1)
    }
}
