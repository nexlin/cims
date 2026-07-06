package com.cims.ue.ptt.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.core.sip.RegState
import com.cims.ue.ptt.R
import kotlinx.coroutines.delay

/**
 * 초기화면(시안 `초기화면.png`) — 동심원 로고 + 기관 로고 + 진행 상태.
 * CIMS SSO 구조라 별도 로그인 화면은 없다: 계정이 있으면 등록 진행 후 자동 진입,
 * 없으면 "CIMS 로그인 열기" 버튼(시안의 "로그인 화면으로" 자리)을 노출한다.
 */
@Composable
fun SplashScreen(st: PttUiState, onReady: () -> Unit) {
    val context = LocalContext.current

    // 등록 완료(또는 계정 있음 + 최소 노출시간 경과) → 자동 진입
    LaunchedEffect(st.reg, st.hasAccount) {
        if (st.hasAccount) {
            delay(if (st.reg is RegState.Registered) 600L else 2500L)
            onReady()
        }
    }

    val pulse = rememberInfiniteTransition(label = "splashPulse")
    val a by pulse.animateFloat(
        initialValue = 0.5f, targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(900, easing = LinearEasing), RepeatMode.Reverse),
        label = "logoAlpha",
    )

    Column(
        Modifier.fillMaxSize().padding(horizontal = 28.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Spacer(Modifier.weight(1.2f))
        Icon(
            painterResource(R.drawable.ic_logo), contentDescription = null,
            tint = Ct.Mint, modifier = Modifier.size(96.dp).alpha(a),
        )
        Spacer(Modifier.height(28.dp))
        Image(
            painterResource(R.drawable.yrt_logo), contentDescription = "기관 로고",
            modifier = Modifier.fillMaxWidth(0.62f),
        )
        Spacer(Modifier.height(10.dp))
        Text("McPTT 앱", color = Ct.Mint, fontSize = 16.sp, fontWeight = FontWeight.Bold)
        Spacer(Modifier.weight(1f))

        when {
            !st.hasAccount -> {
                Text("모든 설정 완료 후 CIMS 계정으로 이용할 수 있습니다.",
                    color = Ct.TextDim, fontSize = 12.sp)
                Spacer(Modifier.height(12.dp))
                MintButton("CIMS 로그인 열기", Modifier.fillMaxWidth()) {
                    val act = context as? android.app.Activity
                    android.accounts.AccountManager.get(context).addAccount(
                        com.cims.ue.core.account.CimsAccounts.ACCOUNT_TYPE,
                        com.cims.ue.core.account.CimsAccounts.TOKEN_MCPTT,
                        null, null, act, null, null)
                }
                Spacer(Modifier.height(10.dp))
                GhostButton("로그인 없이 둘러보기", Modifier.fillMaxWidth(), color = Ct.TextDim) { onReady() }
            }
            else -> {
                val msg = when (st.reg) {
                    is RegState.Registered -> "준비 완료"
                    RegState.Registering -> "서버 연결 중…"
                    is RegState.Failed -> "등록 실패 — 재시도 중"
                    else -> "초기화 중…"
                }
                Text(msg, color = Ct.TextDim, fontSize = 13.sp)
            }
        }
        Spacer(Modifier.height(48.dp))
    }
}
