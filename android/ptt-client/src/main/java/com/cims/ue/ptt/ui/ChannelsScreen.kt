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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
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

private enum class ChannelFilter(val label: String) { ALL("전체"), JOINED("참여 중"), IDLE("미참여") }

/** 전체채널 화면(시안 `전체채널화면.png`) — 필터 칩 + 채널 목록. */
@Composable
fun ChannelsScreen(
    st: PttUiState,
    onOpenChannel: (String) -> Unit,
    onOpenThread: (String) -> Unit,
) {
    var filter by remember { mutableStateOf(ChannelFilter.ALL) }
    LaunchedEffect(st.ctl) { st.ctl?.loadGroups() }   // 진입 시 최신 목록

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 10.dp)) {
        ScreenHeader(
            label = null, title = "전체채널",
            trailing = {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("총 ${st.groups.size}개", color = Ct.TextDim, fontSize = 12.sp)
                    Text("새로고침", color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.Bold,
                        modifier = Modifier.clickable { st.ctl?.loadGroups() })
                }
            },
        )
        Spacer(Modifier.padding(top = 10.dp))

        // 필터 칩(시안의 유형 탭 자리 — 참여 상태 필터)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            ChannelFilter.entries.forEach { f ->
                val sel = f == filter
                Text(
                    f.label, fontSize = 13.sp,
                    color = if (sel) Ct.OnMint else Ct.TextDim,
                    fontWeight = if (sel) FontWeight.Bold else FontWeight.Medium,
                    modifier = Modifier
                        .clip(RoundedCornerShape(50))
                        .background(if (sel) Ct.Mint else Ct.SurfaceHi)
                        .clickable { filter = f }
                        .padding(horizontal = 14.dp, vertical = 6.dp),
                )
            }
        }
        Spacer(Modifier.padding(top = 10.dp))

        val rows = st.groups.map { g -> PttController.bareId(g.uri) to g }
            .filter { (id, _) ->
                when (filter) {
                    ChannelFilter.ALL -> true
                    ChannelFilter.JOINED -> st.session(id) != null
                    ChannelFilter.IDLE -> st.session(id) == null
                }
            }

        if (rows.isEmpty()) {
            Box(Modifier.fillMaxWidth().padding(vertical = 48.dp), contentAlignment = Alignment.Center) {
                Text(
                    if (st.groups.isEmpty()) "채널 목록이 없습니다 — 새로고침을 눌러주세요"
                    else "해당하는 채널이 없습니다",
                    color = Ct.TextFaint, fontSize = 13.sp,
                )
            }
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            items(rows, key = { it.first }) { (id, g) ->
                ChannelRow(st, id, g.displayName ?: id, g.memberCount,
                    onClick = { onOpenChannel(id) }, onMessage = { onOpenThread(id) })
            }
        }
    }
}

/** 채널 행 — 좌측 역할 배지 + 이름/태그 + 우측 메시지 버튼(시안). */
@Composable
private fun ChannelRow(
    st: PttUiState,
    id: String,
    name: String,
    memberCount: Int?,
    onClick: () -> Unit,
    onMessage: () -> Unit,
) {
    val s = st.session(id)
    val (badge, color, dim) = when {
        s?.emergency == true -> Triple("긴급", Ct.Red, Ct.RedDim)
        s?.role == ChannelRole.PRIMARY -> Triple("주", Ct.Mint, Ct.MintDim)
        s != null -> Triple("참", Ct.Gray, Ct.GrayDim)
        else -> Triple(name.take(1), Ct.TextFaint, Ct.GrayDim)
    }
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(Ct.Surface)
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RoleBadge(badge, color, dim)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text(name, color = Ct.Text, fontSize = 15.sp, fontWeight = FontWeight.Bold)
            Row(
                horizontalArrangement = Arrangement.spacedBy(5.dp),
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(top = 4.dp),
            ) {
                TagChip("음성", R.drawable.ic_voice)
                TagChip("${s?.participants?.size ?: memberCount ?: "-"}명")
                when {
                    s?.speaker != null -> PillBadge("● 수신 중", Ct.Mint)
                    s != null -> PillBadge("참여 중", Ct.Gray)
                    st.affiliated.contains(id) -> PillBadge("가입", Ct.Gray)
                }
            }
        }
        Box(
            Modifier.size(38.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                .clickable(onClick = onMessage),
            contentAlignment = Alignment.Center,
        ) {
            Icon(painterResource(R.drawable.ic_message), contentDescription = "메시지",
                tint = Ct.Mint, modifier = Modifier.size(17.dp))
        }
    }
}
