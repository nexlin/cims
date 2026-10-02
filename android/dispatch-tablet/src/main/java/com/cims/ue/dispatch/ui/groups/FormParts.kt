// 카드·폼 조각 — [PTT 그룹]·[관리] 화면이 같이 쓴다 (android_dispatch_tablet.md §6.12·§6.13)
//
// Windows 관제 앱의 같은 이름 스타일(`Themes/Controls.xaml` — `Card`·`ListCard`·`FormCard`·`FormTitle`·`FormLabel`·`Field.Form`·
// `Switch.Form`)과 같은 모양이다: 화면의 칸 = 흰 카드(모서리 10), 폼의 절 = 옅은 면 카드 + 굵은 제목, 이름표 = 작은 두 번째 글자,
// 입력칸 = 한 줄(40 — 손가락이 닿는 높이). 두 화면의 폼이 같은 낱말로 서도록 여기 한 벌만 둔다. 색은 [Tokens] 에서만 읽는다.
package com.cims.ue.dispatch.ui.groups

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.ui.DisabledAlpha
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type

private val CardShape = RoundedCornerShape(10.dp)
private val FieldShape = RoundedCornerShape(8.dp)

/** 화면의 한 칸 — 흰 카드(본문 면 + 옅은 테두리 + 모서리 10). 목록 카드·상세 카드·폼 카드가 같은 틀이다. */
@Composable
internal fun CardFrame(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    val p = Tokens.palette
    Column(modifier.clip(CardShape).background(p.paper).border(1.dp, p.divider, CardShape), content = content)
}

/** 카드 머리의 제목 — «그룹 12»·«조직 5»·«구성원 31». 수는 두 번째 글자색이다. */
@Composable
internal fun CountTitle(title: String, count: Int, modifier: Modifier = Modifier) {
    val p = Tokens.palette
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Text(title, fontSize = Type.title, fontWeight = FontWeight.Bold, maxLines = 1)
        Text(" $count", fontSize = Type.title, fontWeight = FontWeight.Bold, color = p.ink2, maxLines = 1)
    }
}

/**
 * 폼의 절 카드 — 옅은 면 + 옅은 테두리 + 모서리 10, 굵은 제목 아래 항목들.
 *
 * @param note 제목 옆 보조 글자(«이동»·«유선»).
 * @param trailing 제목 줄 오른쪽 끝(회선 상태 라벨).
 */
@Composable
internal fun FormCard(
    title: String,
    modifier: Modifier = Modifier,
    note: String = "",
    trailing: (@Composable RowScope.() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val p = Tokens.palette
    Column(modifier.fillMaxWidth().clip(CardShape).background(p.canvas).border(1.dp, p.divider, CardShape)
            .padding(start = 16.dp, end = 16.dp, top = 13.dp, bottom = 14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(title, fontSize = Type.strong, fontWeight = FontWeight.Bold, maxLines = 1)
            if (note.isNotEmpty()) Text(note, fontSize = Type.meta, color = p.muted, maxLines = 1,
                modifier = Modifier.padding(start = 7.dp))
            if (trailing != null) { Spacer(Modifier.weight(1f)); trailing() }
        }
        content()
    }
}

/** 이름표 + 그 아래 한 칸. 두 칸을 나란히 둘 때는 `Row` 안에서 `Modifier.weight(1f)` 를 준다. */
@Composable
internal fun Labeled(label: String, modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(3.dp)) {
        Text(label, fontSize = Type.meta, color = Tokens.palette.ink2, maxLines = 1, overflow = TextOverflow.Ellipsis)
        content()
    }
}

/** 나란한 두 칸의 줄 — 폼 절 안에서 되풀이된다. */
@Composable
internal fun FieldRow(modifier: Modifier = Modifier, content: @Composable RowScope.() -> Unit) {
    Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.Bottom, content = content)
}

/** 절 안의 안내 한 줄(작은 보조 글자). */
@Composable
internal fun FormHint(text: String, modifier: Modifier = Modifier) {
    Text(text, modifier, fontSize = Type.meta, color = Tokens.palette.muted)
}

/**
 * 한 줄 입력칸(40) — 흰 면 + 강조 외곽선, 포커스면 남색 외곽선. 잠긴 칸은 띠 색 면에 흐린 글자다.
 *
 * @param mono 번호·id 처럼 글자 폭이 같아야 읽히는 값.
 * @param error 값이 모자라다(필수 비밀번호가 비었다) — 빨강 외곽선.
 */
@Composable
internal fun FormField(
    value: String,
    onValue: (String) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    hint: String = "",
    password: Boolean = false,
    mono: Boolean = false,
    keyboard: KeyboardType = KeyboardType.Text,
    error: Boolean = false,
) {
    val p = Tokens.palette
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    BasicTextField(
        value = value, onValueChange = onValue, enabled = enabled, singleLine = true,
        textStyle = TextStyle(fontSize = Type.body, color = if (enabled) p.ink else p.muted,
            fontFamily = if (mono) FontFamily.Monospace else null),
        cursorBrush = SolidColor(p.primaryLine),
        visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
        keyboardOptions = KeyboardOptions(keyboardType = if (password) KeyboardType.Password else keyboard),
        interactionSource = interaction,
        modifier = modifier.fillMaxWidth().height(40.dp).clip(FieldShape).background(if (enabled) p.paper else p.bar)
            .border(if (focused) 1.5.dp else 1.dp,
                when { error -> p.emg; focused -> p.primaryLine; enabled -> p.edge; else -> p.line }, FieldShape),
        decorationBox = { inner ->
            Box(Modifier.fillMaxSize().padding(horizontal = 11.dp), contentAlignment = Alignment.CenterStart) {
                if (value.isEmpty() && hint.isNotEmpty()) Text(hint, fontSize = Type.body, color = p.faint, maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
                inner()
            }
        })
}

/**
 * 숫자 칸 — 값은 `Int` 로 들고 **글자는 칸이 든다.** 지우는 도중의 빈 칸을 값으로 되돌려 쓰면 지울 수가 없다(30 을 45 로
 * 고치려면 먼저 비워야 한다). 숫자가 되는 순간에만 값을 올리고, 칸을 떠날 때 값으로 되돌려 보인다. 범위는 저장할 때 자른다.
 */
@Composable
internal fun NumberField(value: Int, onValue: (Int) -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true) {
    var text by remember(value) { mutableStateOf(value.toString()) }
    FormField(
        value = text,
        onValue = { v ->
            val digits = v.filter(Char::isDigit).take(9)
            text = digits
            digits.toIntOrNull()?.let(onValue)
        },
        modifier = modifier.onFocusChanged { if (!it.isFocused) text = value.toString() },
        enabled = enabled, mono = true, keyboard = KeyboardType.Number)
}

/**
 * 스위치 한 줄 — 손잡이 + 이름. 줄 전체가 누르는 자리다. 잠긴 스위치는 흐리고 [note] 가 왜 잠겼는지 적는다.
 */
@Composable
internal fun FormSwitch(
    label: String,
    checked: Boolean,
    onChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    note: String = "",
) {
    val p = Tokens.palette
    Column(modifier) {
        Row(Modifier.fillMaxWidth().heightIn(min = 36.dp).clip(RoundedCornerShape(6.dp))
                .toggleable(value = checked, enabled = enabled, role = Role.Switch, onValueChange = onChange)
                .alpha(if (enabled) 1f else DisabledAlpha),
            verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(38.dp, 22.dp).clip(RoundedCornerShape(11.dp)).background(if (checked) p.primary else p.edge)
                    .padding(2.dp),
                contentAlignment = if (checked) Alignment.CenterEnd else Alignment.CenterStart) {
                Box(Modifier.size(18.dp).clip(CircleShape).background(p.onPrimary))
            }
            Text(label, fontSize = Type.body, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(start = 8.dp))
        }
        if (note.isNotEmpty()) Text(note, fontSize = Type.meta, color = p.muted, modifier = Modifier.padding(start = 46.dp))
    }
}

/**
 * 고르는 칸(40) — 누르면 목록이 펴진다. 입력칸과 같은 모양이라 폼 안에서 한 줄로 선다.
 *
 * @param shown 지금 고른 값의 글자. [placeholder] 면 자리 글자색이다(아직 안 골랐다).
 */
@Composable
internal fun <T> FormDropdown(
    shown: String,
    options: List<T>,
    text: (T) -> String,
    onPick: (T) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    placeholder: Boolean = false,
) {
    val p = Tokens.palette
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        Row(Modifier.fillMaxWidth().height(40.dp).clip(FieldShape).background(if (enabled) p.paper else p.bar)
                .border(1.dp, if (open) p.primaryLine else if (enabled) p.edge else p.line, FieldShape)
                .clickable(enabled = enabled) { open = true }
                .padding(start = 11.dp, end = 4.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Text(shown, fontSize = Type.body, maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = if (placeholder || !enabled) p.faint else p.ink, modifier = Modifier.weight(1f))
            Icon(Icons.Filled.ArrowDropDown, contentDescription = null, tint = p.muted, modifier = Modifier.size(22.dp))
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            com.cims.ue.dispatch.ui.ForwardPttKeys()
            options.forEach { o ->
                DropdownMenuItem(text = { Text(text(o), fontSize = Type.body) }, onClick = { open = false; onPick(o) })
            }
        }
    }
}

/** 띠의 색조 — [ERROR] 연한 빨강(오류·경고) / [NOTE] 연한 남색(안내). */
internal enum class Band { ERROR, NOTE }

/** 연한 면 띠 — 카드 머리 아래의 오류·안내, 절 안의 경고. */
@Composable
internal fun SoftBand(text: String, modifier: Modifier = Modifier, kind: Band = Band.ERROR) {
    val p = Tokens.palette
    Box(modifier.fillMaxWidth().clip(FieldShape).background(if (kind == Band.ERROR) p.emgSoft else p.primarySoft)
            .padding(horizontal = 10.dp, vertical = 7.dp)) {
        Text(text, fontSize = Type.meta, color = if (kind == Band.ERROR) p.emgInk else p.primaryInk)
    }
}

/** 검색칸(40) — 돋보기 + 한 줄 입력. */
@Composable
internal fun SearchBox(value: String, onValue: (String) -> Unit, hint: String, modifier: Modifier = Modifier,
                       enabled: Boolean = true) {
    val p = Tokens.palette
    BasicTextField(
        value = value, onValueChange = onValue, enabled = enabled, singleLine = true,
        textStyle = TextStyle(fontSize = Type.body, color = p.ink),
        cursorBrush = SolidColor(p.primaryLine),
        modifier = modifier.fillMaxWidth().height(40.dp).alpha(if (enabled) 1f else DisabledAlpha)
            .clip(FieldShape).background(p.paper).border(1.dp, p.edge, FieldShape),
        decorationBox = { inner ->
            Row(Modifier.fillMaxSize().padding(start = 10.dp, end = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.Search, contentDescription = null, tint = p.muted, modifier = Modifier.size(17.dp))
                Box(Modifier.weight(1f).padding(start = 7.dp), contentAlignment = Alignment.CenterStart) {
                    if (value.isEmpty()) Text(hint, fontSize = Type.body, color = p.faint, maxLines = 1,
                        overflow = TextOverflow.Ellipsis)
                    inner()
                }
            }
        })
}

/** 아이콘 단추(36, 모서리 8) — 카드 머리의 [↻], 줄 끝의 [×]. */
@Composable
internal fun IconSquare(icon: ImageVector, description: String, onClick: () -> Unit, modifier: Modifier = Modifier,
                        enabled: Boolean = true, size: Dp = 36.dp) {
    val p = Tokens.palette
    Box(modifier.size(size).alpha(if (enabled) 1f else DisabledAlpha).clip(RoundedCornerShape(8.dp))
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center) {
        Icon(icon, contentDescription = description, tint = p.ink2, modifier = Modifier.size(18.dp))
    }
}

/**
 * 폼 바닥 — 왼쪽에 오류(없으면 안내), 오른쪽에 [취소][저장]. 스크롤과 무관하게 늘 보인다.
 */
@Composable
internal fun FormFooter(error: String, modifier: Modifier = Modifier, note: String = "",
                        buttons: @Composable RowScope.() -> Unit) {
    val p = Tokens.palette
    Row(modifier.fillMaxWidth()
            .drawBehind { drawLine(p.divider, Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx()) }
            .padding(start = 18.dp, end = 14.dp, top = 10.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(error.ifBlank { note }, fontSize = Type.meta, color = if (error.isNotBlank()) p.emg else p.muted,
            maxLines = 3, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
        buttons()
    }
}

/** 빈 자리의 안내 — 목록이 비었거나 아직 고르지 않았다. */
@Composable
internal fun EmptyNote(text: String, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 24.dp), contentAlignment = Alignment.Center) {
        Text(text, fontSize = Type.body, color = Tokens.palette.muted)
    }
}

/** 세로 여백. */
@Composable
internal fun Gap(height: Dp) = Spacer(Modifier.height(height))

/** 가로 여백. */
@Composable
internal fun GapW(width: Dp) = Spacer(Modifier.width(width))
