package com.cims.ue.ptt.ui

import android.app.Activity
import android.content.Context
import android.content.res.Configuration
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat

/** 화면 테마 — 설정 화면에서 고른다. SYSTEM = 단말의 다크 모드 설정을 따른다. */
enum class ThemeMode { SYSTEM, LIGHT, DARK }

/** 한 테마의 토큰 값. 역할은 [Ct] 와 같다. */
class Palette(
    val bg: Color, val surface: Color, val surfaceHi: Color, val border: Color,
    val mint: Color, val mintDim: Color, val onMint: Color,
    val text: Color, val textDim: Color, val textFaint: Color,
    val red: Color, val redDim: Color, val amber: Color, val amberDim: Color,
    val gray: Color, val grayDim: Color, val navBar: Color,
)

/** 다크 — 시안(assets/pages, 다크 배경 + 민트 액센트) 값 그대로. */
private val DarkPalette = Palette(
    bg = Color(0xFF0D1211), surface = Color(0xFF151C1A), surfaceHi = Color(0xFF1B2422), border = Color(0xFF243230),
    mint = Color(0xFF5EE0C0), mintDim = Color(0xFF163229), onMint = Color(0xFF0C1512),
    text = Color(0xFFECF3F1), textDim = Color(0xFF8FA39E), textFaint = Color(0xFF5E6E6A),
    red = Color(0xFFEF5350), redDim = Color(0xFF3A1B1B), amber = Color(0xFFF5C24B), amberDim = Color(0xFF39301A),
    gray = Color(0xFF8A9995), grayDim = Color(0xFF232B29), navBar = Color(0xFF111917),
)

/** 밝게 — 같은 역할의 밝은 값. 본문·보조 글자·민트 글자/면 조합은 WCAG AA 4.5:1 이상(힌트 TextFaint 만 4.1:1). */
private val LightPalette = Palette(
    bg = Color(0xFFF3F6F5), surface = Color(0xFFFFFFFF), surfaceHi = Color(0xFFEDF2F0), border = Color(0xFFC9D6D2),
    mint = Color(0xFF00705F), mintDim = Color(0xFFD6F0E8), onMint = Color(0xFFFFFFFF),
    text = Color(0xFF10201C), textDim = Color(0xFF4A5E59), textFaint = Color(0xFF6E807B),
    red = Color(0xFFC62828), redDim = Color(0xFFFBE3E3), amber = Color(0xFF8A5A00), amberDim = Color(0xFFFFF0D2),
    gray = Color(0xFF5C6B67), grayDim = Color(0xFFE5EBE9), navBar = Color(0xFFFFFFFF),
)

/**
 * 테마 상태 — 고른 모드(설정, `ui_prefs` 의 `theme_mode`)와 단말 다크 모드(Activity 가 구성에서 읽는다).
 * 둘 다 Compose 상태라 [Ct] 토큰을 읽는 화면은 모드가 바뀌면 다시 그려진다.
 */
object PttThemeState {
    private const val PREFS = "ui_prefs"
    private const val KEY = "theme_mode"

    val mode = mutableStateOf(ThemeMode.SYSTEM)
    val systemDark = mutableStateOf(true)

    val dark: Boolean
        get() = when (mode.value) {
            ThemeMode.SYSTEM -> systemDark.value
            ThemeMode.LIGHT -> false
            ThemeMode.DARK -> true
        }

    internal val palette: Palette get() = if (dark) DarkPalette else LightPalette

    /** 저장된 모드와 단말 다크 모드를 읽는다 — Activity onCreate·onConfigurationChanged 에서. */
    fun load(ctx: Context) {
        val saved = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, null)
        mode.value = ThemeMode.entries.firstOrNull { it.name == saved } ?: ThemeMode.SYSTEM
        systemDark.value = isNight(ctx.resources.configuration)
    }

    fun onConfiguration(cfg: Configuration) { systemDark.value = isNight(cfg) }

    fun set(ctx: Context, m: ThemeMode) {
        mode.value = m
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY, m.name).apply()
    }

    private fun isNight(cfg: Configuration) =
        (cfg.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
}

/**
 * 디자인 토큰 — 화면들은 MaterialTheme 대신 주로 이 토큰을 직접 쓴다(시안 색 충실 재현). 값은 현재 테마([PttThemeState])를 따른다.
 * 영상 칸·전체화면 영상 오버레이는 테마와 무관하게 검정 바탕 위 흰 글자다(영상 원본 위에 얹는 층).
 */
object Ct {
    val Bg: Color get() = PttThemeState.palette.bg                  // 화면 배경
    val Surface: Color get() = PttThemeState.palette.surface        // 카드
    val SurfaceHi: Color get() = PttThemeState.palette.surfaceHi    // 카드 위 요소(입력창·서브카드)
    val Border: Color get() = PttThemeState.palette.border          // 카드/입력 테두리
    val Mint: Color get() = PttThemeState.palette.mint              // 액센트(버튼·활성·배지)
    val MintDim: Color get() = PttThemeState.palette.mintDim        // 액센트 배경(옅은 민트 면)
    val OnMint: Color get() = PttThemeState.palette.onMint          // 민트 버튼 위 텍스트
    val Text: Color get() = PttThemeState.palette.text              // 본문
    val TextDim: Color get() = PttThemeState.palette.textDim        // 보조 텍스트
    val TextFaint: Color get() = PttThemeState.palette.textFaint    // 비활성·힌트
    val Red: Color get() = PttThemeState.palette.red                // 긴급·오류
    val RedDim: Color get() = PttThemeState.palette.redDim
    val Amber: Color get() = PttThemeState.palette.amber            // 주의(발언 요청·연결 중)
    val AmberDim: Color get() = PttThemeState.palette.amberDim
    val Gray: Color get() = PttThemeState.palette.gray              // 일반 우선순위
    val GrayDim: Color get() = PttThemeState.palette.grayDim
    val NavBar: Color get() = PttThemeState.palette.navBar          // 하단 내비 바
}

private fun schemeOf(p: Palette, dark: Boolean) =
    if (dark) darkColorScheme(
        primary = p.mint, onPrimary = p.onMint, secondary = p.mint, onSecondary = p.onMint,
        background = p.bg, onBackground = p.text, surface = p.surface, onSurface = p.text,
        surfaceVariant = p.surfaceHi, onSurfaceVariant = p.textDim, outline = p.border, error = p.red,
    ) else lightColorScheme(
        primary = p.mint, onPrimary = p.onMint, secondary = p.mint, onSecondary = p.onMint,
        background = p.bg, onBackground = p.text, surface = p.surface, onSurface = p.text,
        surfaceVariant = p.surfaceHi, onSurfaceVariant = p.textDim, outline = p.border, error = p.red,
    )

private val shapes = Shapes(
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(18.dp),
)

/** 앱 테마 — [PttThemeState] 의 모드(시스템·밝게·어둡게). 상태 바·내비 바 아이콘 밝기도 테마에 맞춘다. */
@Composable
fun PttTheme(content: @Composable () -> Unit) {
    val dark = PttThemeState.dark
    val p = PttThemeState.palette
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as? Activity)?.window ?: return@SideEffect
            @Suppress("DEPRECATION")
            window.statusBarColor = p.bg.toArgb()
            @Suppress("DEPRECATION")
            window.navigationBarColor = p.navBar.toArgb()
            WindowCompat.getInsetsController(window, view).apply {
                isAppearanceLightStatusBars = !dark
                isAppearanceLightNavigationBars = !dark
            }
        }
    }
    MaterialTheme(colorScheme = schemeOf(p, dark), shapes = shapes, content = content)
}
