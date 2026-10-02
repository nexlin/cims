// 새 PTT 그룹 — 채널 추가에서 한 겹 들어온 사이드 패널 (android_dispatch_tablet.md §6.12, dispatch_desktop_ui.md §3.6)
//
// [채널 추가] 에서 고른 사람이 멤버로 들어와 있다(나 = 의장). 그룹 문서(TS 24.481)를 GMS 에 PUT 한다 — 폼·저장은 [PTT 그룹] 화면과
// **같은 VM**([PttGroupsViewModel])이다. 폼을 두 벌 두면 검증·기본값이 갈린다.
//
// 패널은 **줄인 폼**이다 — 이름 · id · 세션 종류 · 멤버. 나머지(그룹 호·한도·서비스)는 [▸ 고급 설정] 이 **패널 안에서** 편다:
// 데스크톱은 [PTT 그룹] 화면으로 넘어가 같은 폼을 이어 쓰지만, 태블릿은 사람을 고르던 «채널» 면을 떠나지 않고 끝낸다. 펴지는 절은
// [PTT 그룹] 화면 폼과 **같은 절**(GroupFormSections.kt)이라 칸·범위·기본값이 갈리지 않는다. 멤버별 [필수]·[의장] 은 만든 뒤
// [PTT 그룹] 화면의 [편집] 에서 준다.
//
// **세션 종류는 둘뿐이다** — prearranged(편성)·chat(TS 24.481 `<on-network-invite-members>`). 일제 통화는 그룹 종류가 아니라
// 호 속성이라(TS 24.379 §4.12 `<broadcast-ind>`) 여기 없다 — 채널 상세의 [일제 통화] 가 연다.
package com.cims.ue.dispatch.ui.groups

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.ui.Pill
import com.cims.ue.dispatch.ui.PillButton
import com.cims.ue.dispatch.ui.SidePanelFrame
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type

/**
 * 새 그룹 패널 — 폼이 사라지면(저장 성공·다른 곳에서 취소) [onDone] 을 부른다. 저장 성공이면 새 그룹이 곧 «내 채널» 에 선다.
 */
@Composable
fun NewGroupPanel(
    groups: PttGroupsViewModel,
    onBack: () -> Unit,
    onClose: () -> Unit,
    onDone: () -> Unit,
) {
    val form by groups.form.collectAsStateWithLifecycle()
    var seen by remember { mutableStateOf(false) }
    LaunchedEffect(form == null) {
        if (form != null) seen = true
        else if (seen) onDone()
    }
    val f = form ?: return
    NewGroupPanelContent(
        form = f,
        onName = { v -> groups.update { it.copy(name = v) } },
        onSessionType = { v -> groups.update { it.copy(sessionType = v) } },
        onRemove = groups::removeMember,
        onChange = groups::update,
        onBack = onBack,
        onClose = { groups.cancelEdit(); onClose() },
        onCancel = { groups.cancelEdit(); onBack() },
        onSave = groups::save)
}

/** 새 그룹 패널 본문 — **순수 컴포저블**(고급 설정 펼침만 제 상태). */
@Composable
fun NewGroupPanelContent(
    form: EditForm,
    onName: (String) -> Unit = {},
    onSessionType: (String) -> Unit = {},
    onRemove: (String) -> Unit = {},
    onChange: ((EditForm) -> EditForm) -> Unit = {},
    onBack: () -> Unit = {},
    onClose: () -> Unit = {},
    onCancel: () -> Unit = {},
    onSave: () -> Unit = {},
    /** 처음부터 고급 설정을 편 채로 — Preview 가 쓴다. */
    advancedOpen: Boolean = false,
) {
    val p = Tokens.palette
    var advanced by remember { mutableStateOf(advancedOpen) }
    SidePanelFrame(title = "새 PTT 그룹", onBack = onBack, onClose = onClose) {
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Labeled("그룹 이름") { FormField(form.name, onName, hint = "예) 3번 게이트 대응") }
            // id 는 만든 뒤 바꿀 수 없다 — 그룹 URI 가 곧 식별자다(identifier_model: 동작은 불변 id, 표시는 이름).
            Labeled("그룹 id — 만든 뒤 바꿀 수 없음") {
                Box(Modifier.fillMaxWidth().height(40.dp).clip(RoundedCornerShape(8.dp)).background(p.bar)
                        .border(1.dp, p.line, RoundedCornerShape(8.dp)).padding(horizontal = 11.dp),
                    contentAlignment = Alignment.CenterStart) {
                    Text("${form.groupId} (자동)", fontSize = Type.body, color = p.muted, fontFamily = FontFamily.Monospace,
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
            Labeled("세션 종류") { SessionTypePills(form.sessionType, onSessionType) }
            Labeled("멤버 ${form.members.size} (나 포함) — 채널 추가에서 고른 사람") {
                @OptIn(ExperimentalLayoutApi::class)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    form.members.forEach { m ->
                        val strong = m.isMe || m.isChair
                        Box(Modifier.clip(RoundedCornerShape(14.dp)).background(if (strong) p.primarySoft else p.paper)
                                .border(1.dp, if (strong) p.primaryLine else p.line, RoundedCornerShape(14.dp))
                                .clickable(enabled = !m.isMe) { onRemove(m.uri) }
                                .padding(horizontal = 10.dp, vertical = 5.dp)) {
                            Text(m.label + when { m.isChair -> " · 의장"; m.isMe -> ""; else -> " ×" }, fontSize = Type.meta,
                                fontWeight = if (strong) FontWeight.Bold else FontWeight.Normal,
                                color = if (strong) p.primaryInk else p.ink)
                        }
                    }
                }
            }
            // 고급 설정 — 기본값으로 충분하다. 펼치면 [PTT 그룹] 화면 폼과 같은 절(그룹 호 · 허용·한도 · 서비스)이 선다.
            Box(Modifier.fillMaxWidth().height(44.dp).clip(RoundedCornerShape(8.dp)).clickable { advanced = !advanced }
                    .drawBehind {
                        val w = 1.dp.toPx()
                        drawRoundRect(p.faint, topLeft = Offset(w / 2, w / 2),
                            size = androidx.compose.ui.geometry.Size(size.width - w, size.height - w),
                            cornerRadius = CornerRadius(8.dp.toPx()),
                            style = Stroke(w, pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 6f))))
                    }.padding(horizontal = 12.dp),
                contentAlignment = Alignment.CenterStart) {
                Text((if (advanced) "▾" else "▸") + " 고급 설정 — 우선순위 · 그룹 호 · 허용·한도 · 서비스(영상)",
                    fontSize = Type.body, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            if (advanced) {
                PriorityRow(form, onChange)
                GroupCallSection(form, onChange)
                LimitsSection(form, onChange)
                ServiceSection(form, onChange)
                FormHint("멤버별 [필수]·[의장] 은 만든 뒤 [PTT 그룹] 화면의 [편집] 에서 줍니다.")
            }
        }
        FormFooter(error = form.error) {
            PillButton("취소", onCancel, kind = Pill.LINE, height = 44.dp)
            PillButton(if (form.busy) "만드는 중…" else "그룹 만들기", onSave, kind = Pill.INK, height = 44.dp,
                enabled = form.canSave)
        }
    }
}
