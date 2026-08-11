package com.cims.ue.ptt.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.ActiveAlert
import com.cims.ue.ptt.GroupCallState
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.PttController

/** 긴급 상태 배너 — 깜빡이는 붉은 배경 + 그룹/개시자, 개시자에게만 [해제] (서버도 개시자 취소만 수용). */
@Composable
fun EmergencyBanner(e: GroupCallState, ctl: PttController?, modifier: Modifier = Modifier) {
    val blink = rememberInfiniteTransition(label = "emgBlink")
    val a by blink.animateFloat(
        initialValue = 1f, targetValue = 0.55f,
        animationSpec = infiniteRepeatable(tween(600, easing = LinearEasing), RepeatMode.Reverse),
        label = "emgAlpha",
    )
    Row(
        modifier.fillMaxWidth().padding(vertical = 6.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(Ct.Red.copy(alpha = a))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text("🚨 긴급 통화 — ${e.groupId}", color = Color.White,
                fontSize = 15.sp, fontWeight = FontWeight.Bold)
            Text(if (e.emergencyMine) "내가 개시 — 상황 종료 시 해제하세요" else "긴급 통화 수신 중",
                color = Color.White, fontSize = 11.sp)
        }
        if (e.emergencyMine) {
            Text("해제", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.plainClickable { ctl?.cancelEmergency() }.padding(8.dp))
        } else {
            // 수신측 로컬 닫기 — 이 단말의 표시만 제거 (발신측 긴급 상태와 무관, AlertBanner 닫기와 동일 의미)
            Text("닫기", color = Color.White, fontSize = 14.sp,
                modifier = Modifier.plainClickable { ctl?.dismissEmergency(e.groupId) }.padding(8.dp))
        }
    }
}

/** 수신 긴급경보 배너 — 통화 없는 위험 통지(TS 24.379 emergency alert).
 *  발신자의 취소 MESSAGE 로 자동 해제되고, [닫기] 는 이 단말의 표시만 지운다.
 *  세션 긴급 배너(빨강 깜빡임·그룹 표기)와의 시각 구분: 주황 계열·📢·사람 표기. */
@Composable
fun AlertBanner(a: ActiveAlert, groupName: String, onDismiss: () -> Unit, modifier: Modifier = Modifier) {
    Row(
        modifier.fillMaxWidth().padding(vertical = 4.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(Ct.AmberDim)
            .border(1.dp, Ct.Amber, RoundedCornerShape(12.dp))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text("📢 긴급경보 — ${a.userId}", color = Ct.Amber,
                fontSize = 14.sp, fontWeight = FontWeight.Bold)
            Text(groupName, color = Color.White, fontSize = 11.sp)
        }
        Text("닫기", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Bold,
            modifier = Modifier.plainClickable(onDismiss).padding(8.dp))
    }
}

/**
 * 하드웨어 버튼 설정 오버레이 — PTT/SOS 물리 키 학습. "설정" 을 누르면 학습 대기가 되고, 그 뒤 단말의
 * 물리 버튼을 누르면 그 keycode 가 해당 기능으로 매핑·영속된다(신규 기종 대응).
 *
 * 🔑 별도 Dialog 윈도우가 아니라 **같은 Activity 윈도우 안의 오버레이**로 그린다. 러기드 단말의 측면
 * 물리 키(예: W999 SOS=310)는 gamepad-class 입력장치라, 별도 Dialog 윈도우가 포커스를 잡으면 focus
 * 네비게이션에 소비돼 앱으로 오지 않는다. 오버레이면 Activity 가 키 포커스를 유지해 물리 키가
 * MainActivity.dispatchKeyEvent → HwPtt.consumeLearn 으로 그대로 유입된다.
 * (접근성 키 필터(PttKeyService) 활성 시엔 그 경로가 같은 학습 규칙으로 먼저 처리한다.)
 */
@Composable
fun KeyConfigOverlay(onDismiss: () -> Unit) {
    val mapping by HwPtt.mapping.collectAsState()
    val learning by HwPtt.learning.collectAsState()
    val context = LocalContext.current
    fun label(code: Int) = if (code > 0) "keycode $code" else "기본값"

    Box(
        Modifier.fillMaxSize().background(Color(0xCC000000)).plainClickable { onDismiss() },
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.fillMaxWidth().padding(24.dp)
                .clip(RoundedCornerShape(18.dp)).background(Ct.Surface)
                .plainClickable {}
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("하드웨어 버튼 설정", color = Ct.Text, fontSize = 19.sp, fontWeight = FontWeight.Bold)
            Text("측면 물리 버튼을 기능에 매핑합니다. ‘설정’을 누른 뒤 원하는 버튼을 누르세요.",
                color = Ct.TextDim, fontSize = 12.sp)
            KeyConfigRow(
                name = "PTT (발언)", value = label(mapping.ptt),
                learningNow = learning == HwPtt.Kind.PTT,
                onLearn = { HwPtt.startLearn(HwPtt.Kind.PTT) },
            )
            KeyConfigRow(
                name = "SOS (긴급)", value = label(mapping.sos),
                learningNow = learning == HwPtt.Kind.SOS,
                onLearn = { HwPtt.startLearn(HwPtt.Kind.SOS) },
            )
            if (learning != null) {
                Text("⌨ 지금 ${if (learning == HwPtt.Kind.PTT) "PTT" else "SOS"} 로 쓸 버튼을 누르세요…",
                    color = Ct.Amber, fontSize = 13.sp, fontWeight = FontWeight.Bold)
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("기본값으로 초기화", color = Ct.TextDim, fontSize = 13.sp,
                    modifier = Modifier.plainClickable { HwPtt.resetMapping(context) }.padding(6.dp))
                Text("닫기", color = Ct.Mint, fontSize = 13.sp, fontWeight = FontWeight.Bold,
                    modifier = Modifier.plainClickable(onDismiss).padding(6.dp))
            }
        }
    }
}

@Composable
private fun KeyConfigRow(name: String, value: String, learningNow: Boolean, onLearn: () -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(name, color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.Bold)
            Text(value, color = Ct.TextDim, fontSize = 12.sp)
        }
        GhostButton(if (learningNow) "대기 중…" else "설정",
            color = if (learningNow) Ct.Amber else Ct.Mint) { if (!learningNow) onLearn() }
    }
}
