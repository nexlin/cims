package com.cims.ue.volte

import android.app.Activity
import android.content.Context
import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat

/** 화면 테마 — 설정 화면에서 고른다. SYSTEM = 단말의 다크 모드 설정을 따른다(PTT 앱과 같다). */
enum class ThemeMode { SYSTEM, LIGHT, DARK }

/** 한 테마의 토큰 값. 역할은 [Ct] 와 같다. 값은 PTT 앱 팔레트와 같다(스위트 앱이 같은 색으로 보이게). */
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

/** 밝게 — 같은 역할의 밝은 값(WCAG AA 4.5:1 — PTT 앱 LightPalette 와 같다). */
private val LightPalette = Palette(
    bg = Color(0xFFF3F6F5), surface = Color(0xFFFFFFFF), surfaceHi = Color(0xFFEDF2F0), border = Color(0xFFC9D6D2),
    mint = Color(0xFF00705F), mintDim = Color(0xFFD6F0E8), onMint = Color(0xFFFFFFFF),
    text = Color(0xFF10201C), textDim = Color(0xFF4A5E59), textFaint = Color(0xFF6E807B),
    red = Color(0xFFC62828), redDim = Color(0xFFFBE3E3), amber = Color(0xFF8A5A00), amberDim = Color(0xFFFFF0D2),
    gray = Color(0xFF5C6B67), grayDim = Color(0xFFE5EBE9), navBar = Color(0xFFFFFFFF),
)

/**
 * 테마 상태 — 고른 모드(설정 «화면 테마», `ui_prefs` 의 `theme_mode`)와 단말 다크 모드(Activity 가 구성에서 읽는다).
 * 둘 다 Compose 상태라 [Ct] 토큰을 읽는 화면은 모드가 바뀌면 다시 그려진다.
 */
object PhoneThemeState {
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
 * 디자인 토큰 — 시안(assets/pages, 다크 배경 + 민트 액센트) 기준, 값은 현재 테마([PhoneThemeState])를 따른다. 화면들은 MaterialTheme
 * 대신 주로 이 토큰을 직접 쓴다(시안 색 충실 재현). 영상 통화 화면은 테마와 무관하게 검정 바탕이다(영상 위 층).
 */
object Ct {
    val Bg: Color get() = PhoneThemeState.palette.bg                // 화면 배경
    val Surface: Color get() = PhoneThemeState.palette.surface      // 카드
    val SurfaceHi: Color get() = PhoneThemeState.palette.surfaceHi  // 카드 위 요소(입력창·서브카드)
    val Border: Color get() = PhoneThemeState.palette.border        // 카드/입력 테두리
    val Mint: Color get() = PhoneThemeState.palette.mint            // 액센트(버튼·활성·배지)
    val MintDim: Color get() = PhoneThemeState.palette.mintDim      // 액센트 배경(옅은 민트 면)
    val OnMint: Color get() = PhoneThemeState.palette.onMint        // 민트 버튼 위 텍스트
    val Text: Color get() = PhoneThemeState.palette.text            // 본문
    val TextDim: Color get() = PhoneThemeState.palette.textDim      // 보조 텍스트
    val TextFaint: Color get() = PhoneThemeState.palette.textFaint  // 비활성·힌트
    val Red: Color get() = PhoneThemeState.palette.red              // 부재중·오류·종료
    val RedDim: Color get() = PhoneThemeState.palette.redDim
    val Amber: Color get() = PhoneThemeState.palette.amber          // 주의(서버 인증서 만료 임박 등)
    val AmberDim: Color get() = PhoneThemeState.palette.amberDim
    val Gray: Color get() = PhoneThemeState.palette.gray
    val GrayDim: Color get() = PhoneThemeState.palette.grayDim
    val NavBar: Color get() = PhoneThemeState.palette.navBar        // 하단 내비 바
}

private fun schemeOf(p: Palette, dark: Boolean) =
    if (dark) darkColorScheme(
        primary = p.mint, onPrimary = p.onMint, secondary = p.mint, onSecondary = p.onMint,
        background = p.bg, onBackground = p.text, surface = p.surface, onSurface = p.text,
        surfaceVariant = p.surfaceHi, onSurfaceVariant = p.textDim,
        primaryContainer = p.mintDim, onPrimaryContainer = p.mint,       // 발신 말풍선·아바타 면
        secondaryContainer = p.mintDim, onSecondaryContainer = p.mint,
        outline = p.border, outlineVariant = p.border, error = p.red,
    ) else lightColorScheme(
        primary = p.mint, onPrimary = p.onMint, secondary = p.mint, onSecondary = p.onMint,
        background = p.bg, onBackground = p.text, surface = p.surface, onSurface = p.text,
        surfaceVariant = p.surfaceHi, onSurfaceVariant = p.textDim,
        primaryContainer = p.mintDim, onPrimaryContainer = p.mint,
        secondaryContainer = p.mintDim, onSecondaryContainer = p.mint,
        outline = p.border, outlineVariant = p.border, error = p.red,
    )

private val shapes = Shapes(
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(18.dp),
)

/** 시안이 다크 고정 — 시스템 설정과 무관하게 다크 스킴 사용(PTT 앱과 동일). */
@Composable
fun PhoneTheme(content: @Composable () -> Unit) {
    val dark = PhoneThemeState.dark
    val p = PhoneThemeState.palette
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

/** 배지/칩 공통 컴팩트 텍스트 스타일 — includeFontPadding 제거로 상하 여백 최소화. */
fun chipStyle(size: Int = 11) = TextStyle(
    fontSize = size.sp, lineHeight = (size + 1).sp,
    platformStyle = PlatformTextStyle(includeFontPadding = false),
)

/** 화면 공통 헤더 — 위 작은 민트 라벨(맥락) + 큰 제목, 우측 액션 슬롯. */
@Composable
fun ScreenHeader(
    label: String?,
    title: String,
    modifier: Modifier = Modifier,
    subtitle: String? = null,
    trailing: (@Composable () -> Unit)? = null,
) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            if (label != null) {
                Text(label, color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            }
            Text(title, color = Ct.Text, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            if (subtitle != null) {
                Text(subtitle, color = Ct.TextDim, fontSize = 12.sp)
            }
        }
        trailing?.invoke()
    }
}

/** 작은 사각 태그 칩 — 통화이력의 "음성/영상" 류. */
@Composable
fun TagChip(text: String, tint: Color = Ct.TextDim) {
    Text(
        text, color = tint, fontWeight = FontWeight.Medium, style = chipStyle(),
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(Ct.SurfaceHi)
            .padding(horizontal = 7.dp, vertical = 2.dp),
    )
}

/** 필터 알약 칩 — 통화이력 상단 "전체/수신/발신/부재중". 선택=민트 외곽선+민트 글자. */
@Composable
fun FilterPill(text: String, selected: Boolean, onClick: () -> Unit) {
    Text(
        text,
        color = if (selected) Ct.Mint else Ct.TextDim,
        fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
        style = chipStyle(13),
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(if (selected) Ct.MintDim else Ct.SurfaceHi)
            .clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 7.dp),
    )
}

/** 리스트 구분 라벨 — "오늘"/날짜 섹션 타이틀. */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(text, color = Ct.TextDim, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
        modifier = modifier.padding(vertical = 6.dp))
}

// ── 하단 내비게이션 (PTT 앱 AppBottomNav 와 동일한 톤 — 다크 바 + 민트 활성) ──

data class NavItem(val label: String, val icon: ImageVector)

@Composable
fun DarkBottomNav(
    items: List<NavItem>,
    currentIndex: Int,
    badge: Map<Int, Int> = emptyMap(),
    onSelect: (Int) -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Ct.NavBar)
            .navigationBarsPadding()
            .padding(top = 6.dp, bottom = 6.dp),
    ) {
        items.forEachIndexed { i, item ->
            val sel = i == currentIndex
            Column(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(10.dp))
                    .clickable { onSelect(i) }
                    .padding(vertical = 4.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                Box {
                    Icon(item.icon, contentDescription = item.label,
                        tint = if (sel) Ct.Mint else Ct.TextFaint,
                        modifier = Modifier.size(22.dp))
                    val n = badge[i] ?: 0
                    if (n > 0) {
                        Box(
                            Modifier
                                .align(Alignment.TopEnd)
                                .padding(start = 14.dp)
                                .size(15.dp)
                                .clip(CircleShape)
                                .background(Ct.Red),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(if (n > 9) "9+" else "$n", color = Color.White,
                                fontSize = 9.sp, fontWeight = FontWeight.Bold)
                        }
                    }
                }
                Text(item.label, fontSize = 11.sp,
                    color = if (sel) Ct.Mint else Ct.TextFaint,
                    fontWeight = if (sel) FontWeight.Bold else FontWeight.Medium)
            }
        }
    }
}
