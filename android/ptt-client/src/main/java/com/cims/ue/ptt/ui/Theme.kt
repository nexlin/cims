package com.cims.ue.ptt.ui

import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/**
 * 디자인 토큰 — 시안(assets/pages, 다크 배경 + 민트 액센트) 기준.
 * 화면들은 MaterialTheme 대신 주로 이 토큰을 직접 쓴다(시안 색 충실 재현).
 */
object Ct {
    val Bg = Color(0xFF0D1211)          // 화면 배경
    val Surface = Color(0xFF151C1A)     // 카드
    val SurfaceHi = Color(0xFF1B2422)   // 카드 위 요소(입력창·서브카드)
    val Border = Color(0xFF243230)      // 카드/입력 테두리
    val Mint = Color(0xFF5EE0C0)        // 액센트(버튼·활성·배지)
    val MintDim = Color(0xFF163229)     // 액센트 배경(옅은 민트 면)
    val OnMint = Color(0xFF0C1512)      // 민트 버튼 위 텍스트
    val Text = Color(0xFFECF3F1)        // 본문
    val TextDim = Color(0xFF8FA39E)     // 보조 텍스트
    val TextFaint = Color(0xFF5E6E6A)   // 비활성·힌트
    val Red = Color(0xFFEF5350)         // 긴급·오류
    val RedDim = Color(0xFF3A1B1B)
    val Amber = Color(0xFFF5C24B)       // 부채널·주의
    val AmberDim = Color(0xFF39301A)
    val Gray = Color(0xFF8A9995)        // 일반 우선순위
    val GrayDim = Color(0xFF232B29)
}

private val scheme = darkColorScheme(
    primary = Ct.Mint,
    onPrimary = Ct.OnMint,
    secondary = Ct.Mint,
    onSecondary = Ct.OnMint,
    background = Ct.Bg,
    onBackground = Ct.Text,
    surface = Ct.Surface,
    onSurface = Ct.Text,
    surfaceVariant = Ct.SurfaceHi,
    onSurfaceVariant = Ct.TextDim,
    outline = Ct.Border,
    error = Ct.Red,
)

private val shapes = Shapes(
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(18.dp),
)

/** 시안이 다크 고정 — 시스템 설정과 무관하게 다크 스킴 사용. */
@Composable
fun PttTheme(content: @Composable () -> Unit) =
    MaterialTheme(colorScheme = scheme, shapes = shapes, content = content)
