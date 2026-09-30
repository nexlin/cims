// 관제 태블릿 시각 토큰 — 색·모양 (android_dispatch_tablet.md §6.3c)
//
// 정본은 «관제 메뉴 재구성» 시안(회색조 와이어프레임)이다. 화면 코드는 **이 이름만** 쓰고 16진수를 직접 쓰지 않는다 —
// 한 곳에서 바꾸면 전 화면이 같이 바뀌어야 한다. Material3 색 체계에 그대로 싣는 것이 요점이다: 기존 컴포넌트(버튼·칩·
// 메뉴·대화상자)가 `MaterialTheme.colorScheme` 을 읽으므로, 여기서 한 번 맞추면 따로 고치지 않은 화면도 같은 톤이 된다.
//
// **상태 색은 회색조로 누르지 않는다.** 시안은 와이어프레임이라 긴급·임박이 검정뿐이지만, 긴급 = 빨강·임박 = 주황은
// 배너·행·머리가 같은 낱말·같은 색으로 가리키는 계약이다(§6.2a-1). 회색조는 바탕·글자·선·면의 톤에만 쓴다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/**
 * 회색조 단계 — 진한 것에서 옅은 것 순서. 이름은 **역할**이다(밝기가 아니라) — 어두운 테마에서는 같은 역할이
 * 뒤집힌 밝기를 갖는다.
 */
@Immutable
data class Palette(
    /** 글자·아이콘의 기본, 강조 면(선택된 세그먼트·주 버튼·발언 대상 ✓). */
    val ink: Color,
    /** 두 번째 글자 — 카드 2줄(발언자·상태). */
    val ink2: Color,
    /** 보조 글자 — 메타·힌트·꺼진 탭. */
    val muted: Color,
    /** 더 옅은 글자·점선 테두리·꺼진 상태. */
    val faint: Color,
    /** 컨트롤 테두리 — 카드·칩·버튼 외곽. */
    val line: Color,
    /** 구역 구분선·옅은 테두리. */
    val divider: Color,
    /** 행 사이 가는 선. */
    val hair: Color,
    /** 채운 면 — 태그·칩 바탕, 선택된 행, 왼쪽 메뉴 바탕. */
    val fill: Color,
    /** 띠 — 탭 줄·발언 바·선택된 카드 바탕. */
    val bar: Color,
    /** 옅은 바탕 — 거르기 칸. */
    val canvas: Color,
    /** 본문 바탕. */
    val paper: Color,
    /** 강조 면 위의 글자(= 바탕색). */
    val onInk: Color,
    /** 긴급 — 글자·테두리·점. */
    val emergency: Color,
    /** 긴급 면. */
    val emergencyFill: Color,
    /** 임박 위험 — 주황(배너·행·머리 공통, [PerilAmber]). */
    val peril: Color,
    /**
     * 발언 중(발언권 승인) — PTT 버튼·발언 대상 칩. 누르고 있는 동안 «지금 나간다» 를 손끝에서 바로 알아야 해서, 대기 상태의
     * 검정과 갈리는 색을 둔다(긴급 빨강·임박 주황과 겹치지 않는 초록).
     */
    val live: Color,
)

val LightPalette = Palette(
    ink = Color(0xFF1F1F1D), ink2 = Color(0xFF4A4A46), muted = Color(0xFF6E6E68), faint = Color(0xFF9A9A94),
    line = Color(0xFFBDBDB8), divider = Color(0xFFD6D6D1), hair = Color(0xFFEDEDEA), fill = Color(0xFFEDEDEA),
    bar = Color(0xFFF4F4F2), canvas = Color(0xFFFAFAF8), paper = Color(0xFFFFFFFF), onInk = Color(0xFFFFFFFF),
    emergency = Color(0xFFC62828), emergencyFill = Color(0xFFFDECEA), peril = PerilAmber, live = Color(0xFF2E7D32),
)

/** 어두운 테마 — 같은 역할을 뒤집은 밝기로. 강조 면(ink)은 밝은 면 + 어두운 글자가 된다. */
val DarkPalette = Palette(
    ink = Color(0xFFF2F2EF), ink2 = Color(0xFFC9C9C3), muted = Color(0xFF9E9E97), faint = Color(0xFF74746E),
    line = Color(0xFF55554F), divider = Color(0xFF3A3A36), hair = Color(0xFF2B2B28), fill = Color(0xFF2E2E2B),
    bar = Color(0xFF222220), canvas = Color(0xFF1C1C1A), paper = Color(0xFF161615), onInk = Color(0xFF161615),
    emergency = Color(0xFFFF6B6B), emergencyFill = Color(0xFF4A1F1F), peril = PerilAmber, live = Color(0xFF5CB860),
)

private fun Palette.scheme(dark: Boolean): ColorScheme {
    val base = if (dark) darkColorScheme() else lightColorScheme()
    return base.copy(
        primary = ink, onPrimary = onInk, primaryContainer = fill, onPrimaryContainer = ink,
        inversePrimary = paper,
        secondary = ink2, onSecondary = onInk, secondaryContainer = fill, onSecondaryContainer = ink,
        tertiary = muted, onTertiary = onInk, tertiaryContainer = bar, onTertiaryContainer = ink,
        background = paper, onBackground = ink,
        surface = paper, onSurface = ink,
        surfaceVariant = bar, onSurfaceVariant = muted,
        surfaceTint = paper,
        inverseSurface = ink, inverseOnSurface = paper,
        outline = line, outlineVariant = divider,
        surfaceBright = paper, surfaceDim = bar,
        surfaceContainerLowest = paper, surfaceContainerLow = canvas, surfaceContainer = bar,
        surfaceContainerHigh = if (dark) fill else paper, surfaceContainerHighest = fill,
        error = emergency, onError = if (dark) Color(0xFF2B0B0B) else Color.White,
        errorContainer = emergencyFill, onErrorContainer = if (dark) Color(0xFFFFDAD6) else Color(0xFF7A1212),
    )
}

/**
 * 모양 — 시안의 반경. 카드 10 · 행/입력 6~8 · 알약 버튼은 Material3 기본(완전 둥금)을 그대로 쓴다.
 */
private val CimsShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),
    small = RoundedCornerShape(6.dp),
    medium = RoundedCornerShape(10.dp),
    large = RoundedCornerShape(12.dp),
    extraLarge = RoundedCornerShape(16.dp),
)

private val LocalPalette = staticCompositionLocalOf { LightPalette }

/** 지금 테마의 회색조 단계 — `MaterialTheme.colorScheme` 에 없는 역할(bar·fill·faint·canvas)을 여기서 읽는다. */
object Tokens {
    val palette: Palette
        @Composable @ReadOnlyComposable get() = LocalPalette.current
}

/** 앱 테마 — Activity·Preview 가 같은 것을 씌운다(같은 자로 재야 비교가 된다, §6.3). */
@Composable
fun CimsTheme(dark: Boolean = false, content: @Composable () -> Unit) {
    val p = if (dark) DarkPalette else LightPalette
    CompositionLocalProvider(LocalPalette provides p) {
        MaterialTheme(colorScheme = p.scheme(dark), shapes = CimsShapes, content = content)
    }
}
