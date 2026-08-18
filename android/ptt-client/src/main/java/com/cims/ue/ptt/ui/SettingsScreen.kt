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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.core.sip.RegState
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.ListenPolicy
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.PttService

/** 설정 탭(시안 `설정화면.png`) — 프로필/통신 설정/채널 설정/기타. */
@Composable
fun SettingsScreen(
    st: PttUiState,
    svc: PttService?,
    onStopSip: () -> Unit,
    onOpenKeyConfig: () -> Unit,
) {
    Column(
        Modifier.fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        ScreenHeader(label = null, title = "설정")

        // ── 기본 설정: 프로필 ──
        SectionLabel("기본 설정")
        SectionCard {
            val id = st.ctl?.mcpttId?.let { PttController.fmtNumber(PttController.bareId(it)) } ?: "-"
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
            // 출력 라우트(스피커/수화기/이어폰)는 주채널 화면의 오디오 출력 아이콘에서 선택한다.
            val all = st.policy == ListenPolicy.ALL
            ToggleRow("전체 듣기", "끄면 주채널만 수신", all) {
                st.ctl?.setListenPolicy(if (all) ListenPolicy.CHANNELS_ONLY else ListenPolicy.ALL)
            }
            Divider()
            GainRow("스피커 게인", "무전 수신 음량 보강 (통화 중 즉시 반영)", st.spkGain) {
                st.ctl?.setAudioGain(it, st.micGain)
            }
            Divider()
            GainRow("마이크 게인", "무전 송신 음량 보강 (상대가 듣는 크기)", st.micGain) {
                st.ctl?.setAudioGain(st.spkGain, it)
            }
            TransportRow(svc)
        }

        // ── 채널 설정: 하드웨어 버튼 ──
        SectionLabel("채널 설정")
        SectionCard(padding = 4) {
            val mapping by HwPtt.mapping.collectAsState()
            NavRow("하드웨어 버튼 설정",
                "PTT ${label(mapping.ptt)} · SOS ${label(mapping.sos)}", onOpenKeyConfig)
            Divider()
            // 백그라운드 PTT 키 — 접근성 키 필터(PttKeyService) 활성 여부. 상태 flow 재구성 때마다
            // 재조회되므로 설정 앱에서 돌아오면 곧 갱신된다.
            val context = androidx.compose.ui.platform.LocalContext.current
            val bgKeyOn = com.cims.ue.ptt.PttKeyService.isEnabled(context)
            NavRow("백그라운드 PTT 버튼",
                if (bgKeyOn) "사용 중 — 앱이 화면에 없어도 측면 버튼 동작"
                else "꺼짐 — 접근성에서 'CIMS PTT 버튼' 을 켜세요") {
                runCatching {
                    context.startActivity(
                        android.content.Intent(android.provider.Settings.ACTION_ACCESSIBILITY_SETTINGS)
                            .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK))
                }
            }
        }

        // ── 기타 ──
        SectionLabel("기타")
        SectionCard(padding = 4) {
            NavRow("그룹 목록 새로고침", "CSC 에서 다시 조회") { st.ctl?.loadGroups() }
            Divider()
            NavRow("서버 설정 다시 받기", "CSC 에서 접속 정보(포트·전송 프로토콜 목록) 재취득") {
                svc?.refreshProvisioning()
            }
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

/** 설정 행 — 게인 슬라이더(×1.0~×3.0, 0.1 단위). */
@Composable
private fun GainRow(title: String, subtitle: String, value: Float, onChange: (Float) -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(title, color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                Text(subtitle, color = Ct.TextFaint, fontSize = 11.sp, modifier = Modifier.padding(top = 2.dp))
            }
            Text("×%.1f".format(value), color = Ct.Mint, fontSize = 13.sp, fontWeight = FontWeight.Bold)
        }
        androidx.compose.material3.Slider(
            value = value,
            onValueChange = { onChange((it * 10).toInt() / 10f) },   // 0.1 단위 스냅
            valueRange = com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MIN..com.cims.ue.ptt.audio.AudioRoutePrefs.GAIN_MAX,
            colors = androidx.compose.material3.SliderDefaults.colors(
                thumbColor = Ct.Mint, activeTrackColor = Ct.Mint, inactiveTrackColor = Ct.GrayDim),
            modifier = Modifier.fillMaxWidth().height(30.dp),
        )
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

/**
 * 설정 행 — SIP 전송 프로토콜 선택. 서버가 프로비저닝으로 알린 **가용 목록**에서 고른다
 * (서버는 UDP/TCP/TLS 를 동시에 청취하며 강제하지 않는다 — sip_tls_signaling.md §7.1).
 * transport 마다 포트가 다르므로(같은 포트로 평문/TLS 를 겸하지 않는다) 선택 시 포트도 함께 바뀌고,
 * 새 경로로 등록하려면 계정을 다시 만들어야 하므로 [PttService.ensureRegistered] 의 재시작 경로를 탄다.
 * 선택지가 2개 미만이면(목록을 안 주는 구 서버/미프로비저닝) 행을 숨긴다.
 */
@Composable
private fun TransportRow(svc: PttService?) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val store = androidx.compose.runtime.remember { com.cims.ue.core.config.ConfigStore(context) }
    // 저장 설정은 SharedPreferences — 변경 알림이 없으므로 서비스의 configTick 을 구독해 다시 읽는다.
    //   remember 로 한 번만 읽으면 "서버 설정 다시 받기" 후에도 옛 목록이 남는다.
    val tick = svc?.configTick?.collectAsState()?.value ?: 0
    val cfg = androidx.compose.runtime.remember(tick) { store.load() }
    val eps = cfg.transports
    if (eps.size < 2) return
    Divider()
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 10.dp)) {
        Text("SIP 전송 프로토콜", color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        Text("서버 접속 방식 — 바꾸면 앱이 재시작되며 새 경로로 등록합니다",
            color = Ct.TextFaint, fontSize = 11.sp, modifier = Modifier.padding(top = 2.dp))
        Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            eps.forEach { ep ->
                val on = ep.transport == cfg.transport
                Box(
                    Modifier.clip(RoundedCornerShape(8.dp))
                        .background(if (on) Ct.Mint else Ct.GrayDim)
                        .clickable(enabled = !on) {
                            store.saveTransportChoice(ep.transport)
                            svc?.bumpConfigTick()
                            svc?.ensureRegistered()      // 설정 변경 감지 → un-REGISTER 후 재시작
                        }
                        .padding(horizontal = 12.dp, vertical = 7.dp),
                ) {
                    Text("${ep.transport.name} · ${ep.port}",
                        color = if (on) Ct.OnMint else Ct.TextDim,
                        fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        }
    }
}
