package com.cims.ue.volte

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
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * 디자인 토큰 — 시안(assets/pages, 다크 배경 + 민트 액센트) 기준. PTT 앱 Ct 와 동일 값.
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
    val Red = Color(0xFFEF5350)         // 부재중·오류·종료
    val RedDim = Color(0xFF3A1B1B)
    val Gray = Color(0xFF8A9995)
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
    primaryContainer = Ct.MintDim,       // 발신 말풍선·아바타 면
    onPrimaryContainer = Ct.Mint,
    secondaryContainer = Ct.MintDim,
    onSecondaryContainer = Ct.Mint,
    outline = Ct.Border,
    outlineVariant = Ct.Border,
    error = Ct.Red,
)

private val shapes = Shapes(
    small = RoundedCornerShape(10.dp),
    medium = RoundedCornerShape(14.dp),
    large = RoundedCornerShape(18.dp),
)

/** 시안이 다크 고정 — 시스템 설정과 무관하게 다크 스킴 사용(PTT 앱과 동일). */
@Composable
fun PhoneTheme(content: @Composable () -> Unit) =
    MaterialTheme(colorScheme = scheme, shapes = shapes, content = content)

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
            .background(Color(0xFF111917))
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
