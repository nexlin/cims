// 관제 태블릿 시각 토큰 — 색·모양·글자 (android_dispatch_tablet.md §6.3c)
//
// 색의 정본은 **Windows 관제 앱의 토큰**이다(dispatch_desktop_ui.md §3.2 «색» — `windows/dispatch-desktop/Themes/Light.xaml`·
// `Dark.xaml`). 두 앱이 같은 낱말·같은 색으로 같은 것을 가리킨다. 화면 코드는 **이 이름만** 쓰고 16진수를 직접 쓰지 않는다 —
// 한 곳에서 바꾸면 전 화면이 같이 바뀌어야 한다. Material3 색 체계에도 같은 값을 싣는다: 기존 컴포넌트(버튼·스위치·메뉴·
// 대화상자)가 `MaterialTheme.colorScheme` 을 읽으므로, 따로 고치지 않은 화면도 같은 톤이 된다.
//
// **축 셋** — 표면(살짝 푸른 무채) · 브랜드 남색(채운 주 행동·선택) · 상태색(발언·통화 녹색 / 긴급 빨강 / 임박·착신·대표번호
// 주황 / 청취·문자 청록 / 감청·경보 보라 / 보류·전달 파랑). 상태색마다 **네 값** — 기본(점·외곽선) · Ink(글자 — 흰 면·연한 면
// 공통) · Soft(연한 면) · Fill(흰 글자를 얹는 채움). 어둡게에서는 기본·Ink 를 밝게, Fill 을 진하게 둔다(한 값으로 겸하면 글자가
// 흐리거나 채움 위 흰 글자가 뜬다).
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp

/** 아바타 한 색 — 연한 면 + 같은 색의 진한 글자(어둡게는 진한 면 + 밝은 글자). */
@Immutable
data class AvatarHue(val bg: Color, val fg: Color)

/**
 * 색 토큰 — 이름은 **역할**이다(밝기가 아니라). 괄호 안은 Windows 토큰 키(`Brush.*`)다.
 */
@Immutable
data class Palette(
    // ── 표면 ──
    /** 본문·카드 면(Surface). */
    val paper: Color,
    /** 옅은 바탕 — 거르기 칸·표 머리(BgSoft). */
    val canvas: Color,
    /** 띠 — 탭 줄·발언 바(Panel). */
    val bar: Color,
    /** 화면 바탕 — 로그인·[PTT 그룹]·[관리] 의 가장 낮은 면(데스크톱 `Brush.Bg` — 그 위에 카드가 뜬다). */
    val bg: Color,
    /** 채운 면 — 기본 라벨·고른 행·절 카드(Surface2). */
    val fill: Color,
    /** 왼쪽 레일 바탕(Rail). */
    val rail: Color,
    /** 레일에서 고른 항목의 알약(RailActive). */
    val railActive: Color,
    /** 행 사이 가는 선(Line). */
    val hair: Color,
    /** 구역 구분선·옅은 테두리(Border). */
    val divider: Color,
    /** 컨트롤 테두리 — 카드·칩·버튼 외곽(BorderStrong). */
    val line: Color,
    /** 강조 외곽선 — 입력칸·알약 버튼·표 머리 밑줄(Edge). */
    val edge: Color,
    /** 꺼진 점 — 미등록·대기(Wire). */
    val wire: Color,
    // ── 글자 ──
    /** 글자·아이콘의 기본(Text·Ink) — **채움에는 쓰지 않는다**(채움은 [primary]). */
    val ink: Color,
    /** 두 번째 글자 — 카드 2줄·칩 글자(TextSoft). */
    val ink2: Color,
    /** 보조 글자 — 메타·힌트(Muted). */
    val muted: Color,
    /** 더 옅은 글자·점선 테두리·꺼진 ✓(Faint). */
    val faint: Color,
    // ── 브랜드(남색) ──
    /** 채운 주 행동·고른 세그먼트·수 배지·로고(Fill). */
    val primary: Color,
    /** [primary] 위의 글자(OnFill). */
    val onPrimary: Color,
    /** 선택의 연한 면 — 고른 칩·고른 행·고른 카드(PrimarySoft). */
    val primarySoft: Color,
    /** 선택의 글자(PrimaryInk). */
    val primaryInk: Color,
    /** 연한 남색 면의 테두리(PrimaryEdge). */
    val primaryEdge: Color,
    /** 선택의 외곽선·참여 중 점·포커스(Primary) — 어둡게에서는 [primary] 보다 밝다. */
    val primaryLine: Color,
    // ── 발언·통화(녹색) ──
    val talk: Color, val talkSoft: Color, val talkInk: Color, val talkEdge: Color, val talkFill: Color,
    // ── 긴급(빨강) ──
    val emg: Color, val emgSoft: Color, val emgInk: Color, val emgEdge: Color, val emgFill: Color,
    // ── 임박·착신·대표번호(주황) ──
    /** 임박 위험의 채운 면(Peril) — 글자는 [onPeril]. */
    val peril: Color, val perilEdge: Color, val onPeril: Color,
    val ring: Color, val ringSoft: Color, val ringBanner: Color, val ringEdge: Color, val ringInk: Color,
    // ── 보류·전달(파랑) ──
    val held: Color, val heldSoft: Color, val heldInk: Color,
    // ── 감청·경보(보라) ──
    val mon: Color, val monSoft: Color, val monEdge: Color, val monInk: Color, val monFill: Color,
    // ── 청취·문자(청록) ──
    val listen: Color, val listenSoft: Color, val listenInk: Color, val listenFill: Color,
    /** 상태색 채움 위의 글자(OnAccent). */
    val onAccent: Color,
    /** 아바타 8색 — 이름 해시로 고른다([avatarHueOf]). 마지막(7)이 무채 기본이다. */
    val avatars: List<AvatarHue>,
)

val LightPalette = Palette(
    paper = Color(0xFFFFFFFF), canvas = Color(0xFFF8FAFC), bar = Color(0xFFF5F7FC), bg = Color(0xFFF2F4FA), fill = Color(0xFFEEF1F8),
    rail = Color(0xFFECEFF8), railActive = Color(0xFFD9DFFA),
    hair = Color(0xFFEDF0F7), divider = Color(0xFFE3E7F1), line = Color(0xFFC9CFE0), edge = Color(0xFFAAB3CC),
    wire = Color(0xFF828BA0),
    ink = Color(0xFF161A2B), ink2 = Color(0xFF334155), muted = Color(0xFF5F6F86), faint = Color(0xFF7D8CA1),
    primary = Color(0xFF4F46E5), onPrimary = Color(0xFFFFFFFF), primarySoft = Color(0xFFEEF2FF),
    primaryInk = Color(0xFF4338CA), primaryEdge = Color(0xFFC7D2FE), primaryLine = Color(0xFF4F46E5),
    talk = Color(0xFF16A34A), talkSoft = Color(0xFFEAFBF0), talkInk = Color(0xFF15803D), talkEdge = Color(0xFFA7E3BC),
    talkFill = Color(0xFF15803D),
    emg = Color(0xFFC62828), emgSoft = Color(0xFFFDECEA), emgInk = Color(0xFF7A1212), emgEdge = Color(0xFFF5B5B0),
    emgFill = Color(0xFFC62828),
    peril = Color(0xFFF59E0B), perilEdge = Color(0xFFD98A06), onPeril = Color(0xFF161A2B),
    ring = Color(0xFFD97706), ringSoft = Color(0xFFFEF5E6), ringBanner = Color(0xFFFDEBC6), ringEdge = Color(0xFFF5C77A),
    ringInk = Color(0xFF92400E),
    held = Color(0xFF2563EB), heldSoft = Color(0xFFE8F0FF), heldInk = Color(0xFF1D4ED8),
    mon = Color(0xFF7C3AED), monSoft = Color(0xFFF5F0FF), monEdge = Color(0xFFE4D7FE), monInk = Color(0xFF6D28D9),
    monFill = Color(0xFF7C3AED),
    listen = Color(0xFF0F766E), listenSoft = Color(0xFFE3F6F3), listenInk = Color(0xFF0F766E), listenFill = Color(0xFF0F766E),
    onAccent = Color(0xFFFFFFFF),
    avatars = listOf(
        AvatarHue(Color(0xFFE0E7FF), Color(0xFF3730A3)), AvatarHue(Color(0xFFE0F2FE), Color(0xFF075985)),
        AvatarHue(Color(0xFFCCFBF1), Color(0xFF115E59)), AvatarHue(Color(0xFFDCFCE7), Color(0xFF166534)),
        AvatarHue(Color(0xFFFEF3C7), Color(0xFF92400E)), AvatarHue(Color(0xFFFFE4E6), Color(0xFF9F1239)),
        AvatarHue(Color(0xFFEDE9FE), Color(0xFF5B21B6)), AvatarHue(Color(0xFFE2E8F0), Color(0xFF334155))),
)

/** 어두운 테마 — 같은 역할. 상태색 «기본·Ink» 는 밝게, «Fill» 은 진하게 둔다. */
val DarkPalette = Palette(
    paper = Color(0xFF161D2C), canvas = Color(0xFF111827), bar = Color(0xFF10172A), bg = Color(0xFF0B0F17), fill = Color(0xFF1E2738),
    rail = Color(0xFF0D1322), railActive = Color(0xFF262E5E),
    hair = Color(0xFF1F2839), divider = Color(0xFF2A3346), line = Color(0xFF3B4660), edge = Color(0xFF4A5677),
    wire = Color(0xFF5E6A88),
    ink = Color(0xFFE6E9F1), ink2 = Color(0xFFCBD5E1), muted = Color(0xFF93A1B8), faint = Color(0xFF64748B),
    primary = Color(0xFF5B5FEF), onPrimary = Color(0xFFFFFFFF), primarySoft = Color(0xFF232A52),
    primaryInk = Color(0xFFA5B4FC), primaryEdge = Color(0xFF3F4A92), primaryLine = Color(0xFF6366F1),
    talk = Color(0xFF22C55E), talkSoft = Color(0xFF122D1E), talkInk = Color(0xFF4ADE80), talkEdge = Color(0xFF1F5C37),
    talkFill = Color(0xFF15803D),
    emg = Color(0xFFF87171), emgSoft = Color(0xFF361519), emgInk = Color(0xFFFCA5A5), emgEdge = Color(0xFF6B2330),
    emgFill = Color(0xFFDC2626),
    peril = Color(0xFFF59E0B), perilEdge = Color(0xFFB45309), onPeril = Color(0xFF161A2B),
    ring = Color(0xFFFBBF24), ringSoft = Color(0xFF2B2313), ringBanner = Color(0xFF33270F), ringEdge = Color(0xFF6B5217),
    ringInk = Color(0xFFFCD34D),
    held = Color(0xFF60A5FA), heldSoft = Color(0xFF172A46), heldInk = Color(0xFF93C5FD),
    mon = Color(0xFFA78BFA), monSoft = Color(0xFF261F40), monEdge = Color(0xFF433870), monInk = Color(0xFFC4B5FD),
    monFill = Color(0xFF7C3AED),
    listen = Color(0xFF2DD4BF), listenSoft = Color(0xFF122D2B), listenInk = Color(0xFF5EEAD4), listenFill = Color(0xFF0F766E),
    onAccent = Color(0xFFFFFFFF),
    avatars = listOf(
        AvatarHue(Color(0xFF2E2B78), Color(0xFFC7D2FE)), AvatarHue(Color(0xFF0C3F5E), Color(0xFFBAE6FD)),
        AvatarHue(Color(0xFF12443F), Color(0xFF99F6E4)), AvatarHue(Color(0xFF15472A), Color(0xFFBBF7D0)),
        AvatarHue(Color(0xFF5C2D0E), Color(0xFFFDE68A)), AvatarHue(Color(0xFF6A1530), Color(0xFFFECDD3)),
        AvatarHue(Color(0xFF3E1E82), Color(0xFFDDD6FE)), AvatarHue(Color(0xFF2C3A50), Color(0xFFE2E8F0))),
)

/**
 * 이름 → 아바타 색 번호(0~7) — 같은 사람은 어느 목록에서나 같은 색이다. Windows `AvatarHueConverter` 와 **같은 셈**이라
 * 두 앱에서 같은 사람이 같은 색으로 보인다: FNV-1a 32비트(UTF-16 단위) 뒤 7 로 나눈 나머지(0~6). 빈 이름은 무채(7).
 */
fun avatarHueOf(name: String): Int {
    val s = name.trim()
    if (s.isEmpty()) return 7
    var h = 2166136261L
    for (c in s) h = ((h xor c.code.toLong()) * 16777619L) and 0xFFFFFFFFL
    return (h % 7).toInt()
}

private fun Palette.scheme(dark: Boolean): ColorScheme {
    val base = if (dark) darkColorScheme() else lightColorScheme()
    return base.copy(
        primary = primary, onPrimary = onPrimary, primaryContainer = primarySoft, onPrimaryContainer = primaryInk,
        inversePrimary = primaryEdge,
        secondary = ink2, onSecondary = paper, secondaryContainer = fill, onSecondaryContainer = ink,
        tertiary = ring, onTertiary = onAccent, tertiaryContainer = ringBanner, onTertiaryContainer = ringInk,
        background = paper, onBackground = ink,
        surface = paper, onSurface = ink,
        surfaceVariant = fill, onSurfaceVariant = muted,
        surfaceTint = paper,
        inverseSurface = ink, inverseOnSurface = paper,
        outline = line, outlineVariant = divider,
        surfaceBright = paper, surfaceDim = bar,
        surfaceContainerLowest = paper, surfaceContainerLow = if (dark) bar else paper, surfaceContainer = if (dark) fill else paper,
        surfaceContainerHigh = if (dark) fill else paper, surfaceContainerHighest = fill,
        error = emg, onError = if (dark) Color(0xFF2B0B0B) else onAccent,
        errorContainer = emgSoft, onErrorContainer = emgInk,
        scrim = Color.Black,
    )
}

/**
 * 모양 — 카드 10 · 행/입력 6~8 · 알약 버튼은 높이의 절반(각 조각이 직접 준다).
 */
private val CimsShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),
    small = RoundedCornerShape(6.dp),
    medium = RoundedCornerShape(10.dp),
    large = RoundedCornerShape(12.dp),
    extraLarge = RoundedCornerShape(16.dp),
)

/**
 * 글자 모양 — 자간 0 · 줄 간격 글자 크기의 1.35배. Material3 기본은 본문에 줄 간격 24sp·자간 0.5sp 를 얹어, 크기만 바꾼
 * `Text` 도 그 줄 간격을 그대로 가진다 — 카드(120) 한 장에 네 줄이 들지 않고 글자가 넓어진다. 크기는 [Type] 이다.
 */
private val CimsTypography = Typography().run {
    fun TextStyle.plain() = copy(letterSpacing = 0.sp, lineHeight = 1.35.em)
    Typography(
        displayLarge = displayLarge.plain(), displayMedium = displayMedium.plain(), displaySmall = displaySmall.plain(),
        headlineLarge = headlineLarge.plain(), headlineMedium = headlineMedium.plain(), headlineSmall = headlineSmall.plain(),
        titleLarge = titleLarge.plain(), titleMedium = titleMedium.plain(), titleSmall = titleSmall.plain(),
        bodyLarge = bodyLarge.plain(), bodyMedium = bodyMedium.plain(), bodySmall = bodySmall.plain(),
        labelLarge = labelLarge.plain(), labelMedium = labelMedium.plain(), labelSmall = labelSmall.plain(),
    )
}

private val LocalPalette = staticCompositionLocalOf { LightPalette }

/** 지금 테마의 색 토큰 — `MaterialTheme.colorScheme` 에 없는 역할(상태색 네 값·레일·아바타)을 여기서 읽는다. */
object Tokens {
    val palette: Palette
        @Composable @ReadOnlyComposable get() = LocalPalette.current
}

/** 앱 테마 — Activity·Preview 가 같은 것을 씌운다(같은 자로 재야 비교가 된다, §6.3). */
@Composable
fun CimsTheme(dark: Boolean = false, content: @Composable () -> Unit) {
    val p = if (dark) DarkPalette else LightPalette
    CompositionLocalProvider(LocalPalette provides p) {
        MaterialTheme(colorScheme = p.scheme(dark), shapes = CimsShapes, typography = CimsTypography, content = content)
    }
}
