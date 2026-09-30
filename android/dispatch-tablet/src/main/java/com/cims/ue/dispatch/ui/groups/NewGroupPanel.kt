// 새 PTT 그룹 — 채널 추가에서 한 겹 들어온 사이드 패널 (android_dispatch_tablet.md §6.12)
//
// [채널 추가] 에서 고른 사람이 멤버로 들어와 있다(나 = 의장). 그룹 문서(TS 24.481)를 GMS 에 PUT 한다 — 폼·저장은 [PTT 그룹] 화면과
// **같은 VM**([PttGroupsViewModel])이다. 폼을 두 벌 두면 검증·기본값이 갈린다.
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
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.ui.FilterPill
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
) {
    val p = Tokens.palette
    var advanced by remember { mutableStateOf(false) }
    SidePanelFrame(title = "새 PTT 그룹", onBack = onBack, onClose = onClose) {
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)) {
            FieldLabel("그룹 이름") {
                BasicTextField(value = form.name, onValueChange = onName, singleLine = true,
                    textStyle = TextStyle(fontSize = Type.strong, color = p.ink), cursorBrush = SolidColor(p.ink),
                    modifier = Modifier.fillMaxWidth().height(44.dp).clip(RoundedCornerShape(6.dp))
                        .border(1.5.dp, p.ink, RoundedCornerShape(6.dp)).background(p.paper),
                    decorationBox = { inner ->
                        Box(Modifier.fillMaxSize().padding(horizontal = 12.dp), contentAlignment = Alignment.CenterStart) {
                            if (form.name.isEmpty()) Text("예) 3번 게이트 대응", fontSize = Type.strong, color = p.faint)
                            inner()
                        }
                    })
            }
            // id 는 만든 뒤 바꿀 수 없다 — 그룹 URI 가 곧 식별자다(identifier_model: 동작은 불변 id, 표시는 이름).
            FieldLabel("그룹 id — 만든 뒤 바꿀 수 없음") {
                Box(Modifier.fillMaxWidth().height(44.dp).clip(RoundedCornerShape(6.dp)).background(p.bar)
                        .border(1.dp, p.line, RoundedCornerShape(6.dp)).padding(horizontal = 12.dp),
                    contentAlignment = Alignment.CenterStart) {
                    Text("${form.groupId} (자동)", fontSize = Type.strong, color = p.muted)
                }
            }
            FieldLabel("세션 종류") {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf("prearranged" to "편성(prearranged)", "chat" to "채팅(chat)").forEach { (v, label) ->
                        FilterPill(label, form.sessionType == v, onClick = { onSessionType(v) })
                    }
                }
            }
            FieldLabel("멤버 ${form.members.size} (나 포함) — 채널 추가에서 고른 사람") {
                @OptIn(ExperimentalLayoutApi::class)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    form.members.forEach { m ->
                        val strong = m.isMe || m.isChair
                        Box(Modifier.clip(RoundedCornerShape(14.dp)).background(if (strong) p.ink else p.paper)
                                .border(1.dp, p.ink, RoundedCornerShape(14.dp))
                                .clickable(enabled = !m.isMe) { onRemove(m.uri) }
                                .padding(horizontal = 10.dp, vertical = 5.dp)) {
                            Text(m.label + when { m.isChair -> " · 의장"; m.isMe -> ""; else -> " ×" }, fontSize = Type.meta,
                                color = if (strong) p.onInk else p.ink)
                        }
                    }
                }
            }
            // 고급 설정 — 기본값으로 충분하다. 펼치면 TS 24.481 그룹 문서의 기능 스위치가 선다.
            Box(Modifier.fillMaxWidth().height(44.dp).clip(RoundedCornerShape(6.dp)).clickable { advanced = !advanced }
                    .drawBehind {
                        val w = 1.dp.toPx()
                        drawRoundRect(p.faint, topLeft = Offset(w / 2, w / 2),
                            size = androidx.compose.ui.geometry.Size(size.width - w, size.height - w),
                            cornerRadius = CornerRadius(6.dp.toPx()),
                            style = Stroke(w, pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 6f))))
                    }.padding(horizontal = 12.dp),
                contentAlignment = Alignment.CenterStart) {
                Text((if (advanced) "▾" else "▸") + " 고급 설정 — SDS · 파일 · 영상 · 암호화 · 긴급 · 우선순위",
                    fontSize = Type.body)
            }
            if (advanced) Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                SwitchLine("그룹 문자(SDS)", form.allowSds) { v -> onChange { it.copy(allowSds = v) } }
                SwitchLine("파일 전송(FD)", form.allowFd) { v -> onChange { it.copy(allowFd = v) } }
                SwitchLine("영상", form.videoEnabled) { v -> onChange { it.copy(videoEnabled = v) } }
                SwitchLine("종단간 암호화", form.encryption) { v -> onChange { it.copy(encryption = v) } }
                SwitchLine("긴급 통화", form.emergencyCall) { v -> onChange { it.copy(emergencyCall = v) } }
                SwitchLine("긴급 경보", form.emergencyAlert) { v -> onChange { it.copy(emergencyAlert = v) } }
                SwitchLine("가입(affiliation) 필요", form.requireAffiliation) { v -> onChange { it.copy(requireAffiliation = v) } }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("우선순위 ${form.priority}", fontSize = Type.body, modifier = Modifier.weight(1f))
                    PillButton("−", { onChange { it.copy(priority = (it.priority - 1).coerceAtLeast(0)) } }, height = 32.dp)
                    Spacer(Modifier.width(6.dp))
                    PillButton("+", { onChange { it.copy(priority = (it.priority + 1).coerceAtMost(15)) } }, height = 32.dp)
                }
            }
            if (form.error.isNotEmpty()) Text(form.error, fontSize = Type.meta, color = p.emergency)
        }
        Row(
            Modifier.fillMaxWidth()
                .drawBehind { drawLine(p.divider, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
                .padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End), verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = onCancel) { Text("취소", fontSize = Type.strong) }
            PillButton(if (form.busy) "만드는 중…" else "그룹 만들기", onSave, height = 44.dp, filled = true,
                enabled = form.canSave)
        }
    }
}

@Composable
private fun FieldLabel(label: String, body: @Composable () -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(label, fontSize = Type.meta, color = Tokens.palette.ink2)
        body()
    }
}

@Composable
private fun SwitchLine(label: String, on: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().height(40.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(label, fontSize = Type.body, fontWeight = FontWeight.Normal, modifier = Modifier.weight(1f))
        Switch(checked = on, onCheckedChange = onChange)
    }
}
