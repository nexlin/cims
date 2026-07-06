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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.ChannelRole
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.R

/**
 * 채널 상세(시안 `채널선택화면.png`) — 채널 상태 카드 + 접속 중/오프라인 구성원 목록 +
 * 하단 [주채널 설정]/[부채널 설정]/(참여·나가기).
 */
@Composable
fun ChannelDetailScreen(
    st: PttUiState,
    groupId: String,
    onBack: () -> Unit,
    onOpenThread: (String) -> Unit,
) {
    val s = st.session(groupId)
    val name = st.groupName(groupId)
    val joined = s != null

    Column(Modifier.fillMaxSize().statusBarsPadding().padding(horizontal = 16.dp, vertical = 10.dp)) {
        // 상단바 — 뒤로 + 채널명 + 역할 배지
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Box(
                Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                    .clickable(onClick = onBack),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_back), contentDescription = "뒤로",
                    tint = Ct.Text, modifier = Modifier.size(18.dp))
            }
            Text(name, color = Ct.Text, fontSize = 19.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.weight(1f))
            when {
                s?.emergency == true -> PillBadge("긴급", Ct.Red, filled = true)
                s?.role == ChannelRole.PRIMARY -> PillBadge("주채널", Ct.Mint, filled = true)
                s?.role == ChannelRole.SECONDARY -> PillBadge("부채널", Ct.Amber, filled = true)
                joined -> PillBadge("참여 중", Ct.Gray)
            }
        }
        Spacer(Modifier.height(12.dp))

        // 안내 배너 — "현재 주채널로 설정되어 있습니다" 류(시안)
        val notice = when {
            s?.role == ChannelRole.PRIMARY -> "현재 주채널로 설정되어 있습니다." to Ct.Mint
            s?.role == ChannelRole.SECONDARY -> "현재 부채널로 설정되어 있습니다." to Ct.Amber
            joined -> "참여 중입니다. 주채널로 설정하면 발언할 수 있습니다." to Ct.TextDim
            else -> "주채널로 설정하면 참여와 함께 발언할 수 있습니다." to Ct.TextDim
        }
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp))
                .background(notice.second.copy(alpha = 0.10f))
                .padding(horizontal = 12.dp, vertical = 9.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            StatusDot(notice.second, 6)
            Spacer(Modifier.width(8.dp))
            Text(notice.first, color = notice.second, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
        }
        Spacer(Modifier.height(12.dp))

        // 채널 상태 카드
        SectionCard {
            SectionLabel("채널 상태")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    val total = s?.participants?.size
                        ?: st.groups.firstOrNull { PttController.bareId(it.uri) == groupId }?.memberCount
                    Text(
                        if (joined) "구성원 ${total ?: "-"}명 · 접속 중" else "구성원 ${total ?: "-"}명",
                        color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
                    )
                    s?.speaker?.let { sp ->
                        Text(
                            (if (sp.self) "내가" else PttController.bareId(sp.id)) + " 발언 중",
                            color = if (sp.self) Ct.Mint else Ct.Amber, fontSize = 12.sp,
                            modifier = Modifier.padding(top = 3.dp),
                        )
                    }
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box(
                        Modifier.size(38.dp).clip(RoundedCornerShape(10.dp))
                            .background(if (s?.speaker != null) Ct.MintDim else Ct.SurfaceHi),
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(painterResource(R.drawable.ic_voice), contentDescription = "음성",
                            tint = if (s?.speaker != null) Ct.Mint else Ct.TextFaint,
                            modifier = Modifier.size(17.dp))
                    }
                    Box(
                        Modifier.size(38.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                            .clickable { onOpenThread(groupId) },
                        contentAlignment = Alignment.Center,
                    ) {
                        Icon(painterResource(R.drawable.ic_message), contentDescription = "메시지",
                            tint = Ct.Mint, modifier = Modifier.size(17.dp))
                    }
                }
            }
        }
        Spacer(Modifier.height(12.dp))

        // 구성원 목록 — 접속 중(conference-info 참가자) / 그 외
        val me = st.ctl?.mcpttId?.let { PttController.bareId(it) }
        val speakerId = s?.speaker?.let { PttController.bareId(it.id) }
        val online = s?.participants?.entries?.sortedBy { it.key } ?: emptyList()

        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (online.isNotEmpty()) {
                items(listOf("접속 중")) { SectionLabel("접속 중 (${online.size})") }
                items(online.map { it.key to it.value }, key = { it.first }) { (pid, status) ->
                    val speaking = pid == speakerId
                    val pending = status.equals("pending", true)
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp))
                            .background(if (speaking) Ct.MintDim else Ct.Surface)
                            .padding(horizontal = 12.dp, vertical = 9.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        InitialAvatar(pid, active = !pending)
                        Column(Modifier.weight(1f)) {
                            Text(
                                if (pid == me) "$pid (나)" else pid,
                                color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
                            )
                            Text(
                                when {
                                    speaking -> "송신 중"
                                    pending -> "연결 중"
                                    else -> "대기"
                                },
                                color = when {
                                    speaking -> Ct.Mint
                                    pending -> Ct.TextFaint
                                    else -> Ct.TextDim
                                },
                                fontSize = 11.sp,
                            )
                        }
                        if (speaking) {
                            Icon(painterResource(R.drawable.ic_level_meter), contentDescription = null,
                                tint = Ct.Mint, modifier = Modifier.size(16.dp))
                        }
                    }
                }
            } else if (!joined) {
                items(listOf("hint")) {
                    Box(Modifier.fillMaxWidth().padding(vertical = 32.dp), contentAlignment = Alignment.Center) {
                        Text("참여하면 접속 구성원이 표시됩니다", color = Ct.TextFaint, fontSize = 13.sp)
                    }
                }
            }
        }

        // 하단 액션 — 시안: [주채널 설정] (+부채널 설정)
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            when {
                !joined -> {
                    MintButton("주채널로 참여", Modifier.weight(1f)) {
                        st.ctl?.joinGroupCall(groupId)
                        st.ctl?.setPrimary(groupId)
                    }
                    GhostButton("참여만", Modifier.weight(0.6f)) { st.ctl?.joinGroupCall(groupId) }
                }
                s?.role == ChannelRole.PRIMARY -> {
                    GhostButton("나가기", Modifier.weight(1f), color = Ct.Red) {
                        st.ctl?.leaveGroup(groupId); onBack()
                    }
                }
                else -> {
                    MintButton("주채널 설정", Modifier.weight(1f)) { st.ctl?.setPrimary(groupId) }
                    GhostButton(
                        if (s?.role == ChannelRole.SECONDARY) "부채널 해제" else "부채널 설정",
                        Modifier.weight(1f), color = Ct.Amber,
                    ) { st.ctl?.toggleSecondary(groupId) }
                    GhostButton("나가기", color = Ct.Red) { st.ctl?.leaveGroup(groupId); onBack() }
                }
            }
        }
        Spacer(Modifier.height(10.dp))
    }
}
