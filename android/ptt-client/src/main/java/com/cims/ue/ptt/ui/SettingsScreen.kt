package com.cims.ue.ptt.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.core.sip.RegState
import com.cims.ue.core.sip.SipController
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.ListenPolicy
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.PttService
import com.cims.ue.ptt.R

/** 설정 화면(시안 `설정화면.png`) — 프로필/통신 설정/채널 설정/기타. */
@Composable
fun SettingsScreen(
    st: PttUiState,
    svc: PttService?,
    onBack: () -> Unit,
    onStopSip: () -> Unit,
    onOpenKeyConfig: () -> Unit,
) {
    Column(
        Modifier.fillMaxSize().statusBarsPadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Box(
                Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                    .clickable(onClick = onBack),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_back), contentDescription = "뒤로",
                    tint = Ct.Text, modifier = Modifier.size(18.dp))
            }
            Text("설정", color = Ct.Text, fontSize = 22.sp, fontWeight = FontWeight.Bold)
        }

        // ── 기본 설정: 프로필 ──
        SectionLabel("기본 설정")
        SectionCard {
            val id = st.ctl?.mcpttId?.let { PttController.bareId(it) } ?: "-"
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                InitialAvatar(id, active = st.reg is RegState.Registered, size = 42)
                Column(Modifier.weight(1f)) {
                    Text(id, color = Ct.Text, fontSize = 15.sp, fontWeight = FontWeight.Bold)
                    val (c, t) = when (st.reg) {
                        is RegState.Registered -> Ct.Mint to "접속 중"
                        RegState.Registering -> Ct.Amber to "연결 중…"
                        is RegState.Failed -> Ct.Red to "등록 실패"
                        else -> Ct.TextFaint to "미접속"
                    }
                    Row(verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                        modifier = Modifier.padding(top = 3.dp)) {
                        StatusDot(c, 6)
                        Text(t, color = c, fontSize = 12.sp)
                    }
                }
                if (st.reg is RegState.Registered) {
                    Text("해제", color = Ct.Red, fontSize = 13.sp, fontWeight = FontWeight.Bold,
                        modifier = Modifier.clickable(onClick = onStopSip))
                } else {
                    Text("등록", color = Ct.Mint, fontSize = 13.sp, fontWeight = FontWeight.Bold,
                        modifier = Modifier.clickable { svc?.ensureRegistered() })
                }
            }
        }

        // ── 통신 설정 ──
        SectionLabel("통신 설정")
        SectionCard(padding = 4) {
            val speakerOn = st.route == SipController.AUDIO_ROUTE_SPEAKER
            ToggleRow("스피커 출력", "끄면 수화구·이어폰 자동", speakerOn) {
                st.ctl?.setAudioRoute(
                    if (speakerOn) SipController.AUDIO_ROUTE_DEFAULT else SipController.AUDIO_ROUTE_SPEAKER)
            }
            Divider()
            val all = st.policy == ListenPolicy.ALL
            ToggleRow("전체 듣기", "끄면 주·부채널만 수신", all) {
                st.ctl?.setListenPolicy(if (all) ListenPolicy.CHANNELS_ONLY else ListenPolicy.ALL)
            }
        }

        // ── 채널 설정: 하드웨어 버튼 ──
        SectionLabel("채널 설정")
        SectionCard(padding = 4) {
            val mapping by HwPtt.mapping.collectAsState()
            NavRow("하드웨어 버튼 설정",
                "PTT ${label(mapping.ptt)} · SOS ${label(mapping.sos)}", onOpenKeyConfig)
        }

        // ── 기타 ──
        SectionLabel("기타")
        SectionCard(padding = 4) {
            NavRow("그룹 목록 새로고침", "CSC 에서 다시 조회") { st.ctl?.loadGroups() }
            Divider()
            val context = androidx.compose.ui.platform.LocalContext.current
            val ver = androidx.compose.runtime.remember {
                runCatching {
                    context.packageManager.getPackageInfo(context.packageName, 0).versionName
                }.getOrNull() ?: "-"
            }
            Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 13.dp),
                verticalAlignment = Alignment.CenterVertically) {
                Text("버전", color = Ct.Text, fontSize = 14.sp, modifier = Modifier.weight(1f))
                Text(ver, color = Ct.TextDim, fontSize = 13.sp)
            }
        }

        Text(st.status, color = Ct.TextFaint, fontSize = 11.sp,
            modifier = Modifier.fillMaxWidth().padding(bottom = 12.dp))
    }
}

private fun label(code: Int) = if (code > 0) "keycode $code" else "기본값"

@Composable
private fun Divider() {
    Box(Modifier.fillMaxWidth().height(1.dp).padding(horizontal = 12.dp).background(Ct.Border))
}

/** 설정 행 — 제목/설명 + 민트 토글 스위치(시안 스타일). */
@Composable
private fun ToggleRow(title: String, subtitle: String, on: Boolean, onToggle: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onToggle)
            .padding(horizontal = 12.dp, vertical = 11.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            Text(subtitle, color = Ct.TextFaint, fontSize = 11.sp, modifier = Modifier.padding(top = 2.dp))
        }
        // 토글 스위치 — 시안 스펙(50×28, 켜짐=민트/꺼짐=어두움, 핸들 22)
        Box(
            Modifier.size(width = 50.dp, height = 28.dp).clip(RoundedCornerShape(14.dp))
                .background(if (on) Ct.Mint else Ct.GrayDim),
            contentAlignment = if (on) Alignment.CenterEnd else Alignment.CenterStart,
        ) {
            Box(Modifier.padding(horizontal = 3.dp).size(22.dp).clip(CircleShape)
                .background(if (on) Ct.OnMint else Ct.TextFaint))
        }
    }
}

/** 설정 행 — 이동형(꺾쇠). */
@Composable
private fun NavRow(title: String, subtitle: String, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 13.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            Text(subtitle, color = Ct.TextFaint, fontSize = 11.sp, modifier = Modifier.padding(top = 2.dp))
        }
        Text("›", color = Ct.TextFaint, fontSize = 18.sp)
        Spacer(Modifier.width(2.dp))
    }
}
