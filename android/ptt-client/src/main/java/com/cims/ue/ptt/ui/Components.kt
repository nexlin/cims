package com.cims.ue.ptt.ui

import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.R

/** 배지/칩 공통 컴팩트 텍스트 스타일 — includeFontPadding 제거로 상하 여백 최소화. */
private fun chipStyle(size: Int = 11) = TextStyle(
    fontSize = size.sp, lineHeight = (size + 1).sp,
    platformStyle = PlatformTextStyle(includeFontPadding = false),
)

/** 클릭 리플 없는 clickable (시안의 플랫한 다크 UI 톤 유지용은 기본 리플 사용, 오버레이 스크림에만 사용). */
fun Modifier.plainClickable(onClick: () -> Unit): Modifier = this.then(
    Modifier.clickable(indication = null, interactionSource = MutableInteractionSource(), onClick = onClick),
)

/** 채팅 입력바 첨부(사진/동영상) 버튼 — 시스템 포토 피커.
 *  미디어 전송은 서버 업로드 경로 연동 후 지원(현재는 선택 시 안내만). */
@Composable
fun AttachButton(size: Int = 38) {
    val context = LocalContext.current
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri != null) {
            Toast.makeText(context, "사진·동영상 전송은 서버 연동 후 지원됩니다", Toast.LENGTH_SHORT).show()
        }
    }
    Box(
        Modifier.size(size.dp).clip(CircleShape).background(Ct.SurfaceHi)
            .clickable {
                picker.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageAndVideo))
            },
        contentAlignment = Alignment.Center,
    ) {
        Icon(painterResource(R.drawable.ic_attach), contentDescription = "사진·동영상 첨부",
            tint = Ct.TextDim, modifier = Modifier.size((size * 9 / 20).dp))
    }
}

/** 화면 공통 헤더 — 위 작은 민트 라벨(기관/맥락) + 큰 제목, 우측 액션 슬롯. */
@Composable
fun ScreenHeader(
    label: String?,
    title: String,
    modifier: Modifier = Modifier,
    trailing: (@Composable () -> Unit)? = null,
) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            if (label != null) {
                Text(label, color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            }
            Text(title, color = Ct.Text, fontSize = 22.sp, fontWeight = FontWeight.Bold)
        }
        trailing?.invoke()
    }
}

/** 카드 — 시안의 어두운 라운드 카드(테두리 미세). */
@Composable
fun SectionCard(
    modifier: Modifier = Modifier,
    padding: Int = 14,
    content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit,
) {
    Column(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(16.dp))
            .background(Ct.Surface)
            .border(1.dp, Ct.Border, RoundedCornerShape(16.dp))
            .padding(padding.dp),
        content = content,
    )
}

/** 작은 사각 태그 칩 — 시안의 "영상/열차/구성원 (5)" 류. */
@Composable
fun TagChip(text: String, iconRes: Int? = null, tint: Color = Ct.TextDim) {
    Row(
        Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(Ct.SurfaceHi)
            .padding(horizontal = 7.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        if (iconRes != null) {
            Icon(painterResource(iconRes), contentDescription = null, tint = tint, modifier = Modifier.size(11.dp))
        }
        Text(text, color = tint, fontWeight = FontWeight.Medium, style = chipStyle())
    }
}

/** 소형 사각 배지 — "주"/"일반" 등 한두 글자, 모서리 미세 라운드 + 상하 여백 최소(시안). */
@Composable
fun SquareBadge(text: String, color: Color) {
    Text(
        text, color = color, fontWeight = FontWeight.Bold,
        style = chipStyle(),
        modifier = Modifier
            .clip(RoundedCornerShape(4.dp))
            .border(1.dp, color.copy(alpha = 0.55f), RoundedCornerShape(4.dp))
            .background(color.copy(alpha = 0.14f))
            .padding(horizontal = 5.dp, vertical = 1.dp),
    )
}

/** 채널 우선순위/역할 배지 — 시안 좌측의 컬러 사각 배지(P1 붉은·P3 노랑 등). */
@Composable
fun RoleBadge(text: String, color: Color, dim: Color) {
    Box(
        Modifier
            .size(26.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(dim)
            .border(1.dp, color.copy(alpha = 0.55f), RoundedCornerShape(8.dp)),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = color, fontSize = 12.sp, fontWeight = FontWeight.Bold)
    }
}

/** 알약형 상태 배지 — "● 수신 중"/"접속" 류. */
@Composable
fun PillBadge(text: String, color: Color = Ct.Mint, filled: Boolean = false) {
    Text(
        text,
        color = if (filled) Ct.OnMint else color,
        fontWeight = FontWeight.SemiBold,
        style = chipStyle(),
        modifier = Modifier
            .clip(RoundedCornerShape(50))
            .background(if (filled) color else color.copy(alpha = 0.14f))
            .padding(horizontal = 8.dp, vertical = 2.dp),
    )
}

@Composable
fun StatusDot(color: Color, size: Int = 7) {
    Box(Modifier.size(size.dp).clip(CircleShape).background(color))
}

/** 큰 민트 주버튼 — 시안 "로그인"/"주채널 설정". */
@Composable
fun MintButton(text: String, modifier: Modifier = Modifier, enabled: Boolean = true, onClick: () -> Unit) {
    Box(
        modifier
            .height(50.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(if (enabled) Ct.Mint else Ct.GrayDim)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = if (enabled) Ct.OnMint else Ct.TextFaint,
            fontSize = 15.sp, fontWeight = FontWeight.Bold, modifier = Modifier.padding(horizontal = 18.dp))
    }
}

/** 보조 버튼 — 어두운 면 + 테두리(나가기·참여만 등). */
@Composable
fun GhostButton(text: String, modifier: Modifier = Modifier, color: Color = Ct.Text, onClick: () -> Unit) {
    Box(
        modifier
            .height(50.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(Ct.SurfaceHi)
            .border(1.dp, Ct.Border, RoundedCornerShape(14.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = color, fontSize = 15.sp, fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 18.dp))
    }
}

/** 이름 아바타 — 시안 참여자 행의 원형 이니셜. */
@Composable
fun InitialAvatar(name: String, active: Boolean = true, size: Int = 36) {
    Box(
        Modifier
            .size(size.dp)
            .clip(CircleShape)
            .background(if (active) Ct.MintDim else Ct.GrayDim)
            .border(1.dp, if (active) Ct.Mint.copy(alpha = 0.5f) else Ct.Border, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Text(name.take(1), color = if (active) Ct.Mint else Ct.TextFaint,
            fontSize = (size * 0.42).sp, fontWeight = FontWeight.Bold)
    }
}

// ── 하단 내비게이션 ──

enum class Tab(val label: String, val icon: Int) {
    MAIN("주채널", R.drawable.ic_nav_main),
    CHANNELS("전체채널", R.drawable.ic_nav_channels),
    MESSAGES("메시지", R.drawable.ic_nav_message),
    SETTINGS("설정", R.drawable.ic_nav_settings),
}

/** 하단 내비 4탭 — 시안: 다크 바, 활성=민트 아이콘+라벨. [badge]=탭별 뱃지 수(메시지 안읽음). */
@Composable
fun AppBottomNav(current: Tab, badge: Map<Tab, Int> = emptyMap(), onSelect: (Tab) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(Color(0xFF111917))
            .navigationBarsPadding()
            .padding(top = 6.dp, bottom = 6.dp),
    ) {
        Tab.entries.forEach { tab ->
            val sel = tab == current
            Column(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(10.dp))
                    .clickable { onSelect(tab) }
                    .padding(vertical = 4.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(3.dp),
            ) {
                Box {
                    Icon(
                        painterResource(tab.icon), contentDescription = tab.label,
                        tint = if (sel) Ct.Mint else Ct.TextFaint,
                        modifier = Modifier.size(22.dp),
                    )
                    val n = badge[tab] ?: 0
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
                Text(tab.label, fontSize = 11.sp,
                    color = if (sel) Ct.Mint else Ct.TextFaint,
                    fontWeight = if (sel) FontWeight.Bold else FontWeight.Medium)
            }
        }
    }
}

/** 리스트 구분 라벨 — "접속 중"/"오프라인"/"기본 설정" 섹션 타이틀. */
@Composable
fun SectionLabel(text: String, modifier: Modifier = Modifier) {
    Text(text, color = Ct.TextDim, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
        modifier = modifier.padding(vertical = 6.dp))
}
