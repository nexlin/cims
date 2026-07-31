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
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
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
import com.cims.ue.ptt.csc.GroupSummary
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** 전체채널 화면(시안 `전체채널화면.png`) — 채널 목록, 당겨서 새로고침. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChannelsScreen(
    st: PttUiState,
    onOpenChannel: (String) -> Unit,
    onOpenThread: (String) -> Unit,
) {
    LaunchedEffect(st.ctl) { st.ctl?.loadGroups() }   // 진입 시 최신 목록
    // 그룹 속성(우선순위·영상)은 표준 그룹 문서(TS 24.481)에서 — ETag 304 캐시라 저비용
    LaunchedEffect(st.groups) { st.groups.forEach { st.ctl?.loadGroupDetail(PttController.bareId(it.uri)) } }

    val scope = rememberCoroutineScope()
    var refreshing by remember { mutableStateOf(false) }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 10.dp)) {
        ScreenHeader(
            label = null, title = "전체채널",
            trailing = { Text("총 ${st.groups.size}개", color = Ct.TextDim, fontSize = 12.sp) },
        )
        Spacer(Modifier.padding(top = 10.dp))

        val rows = st.groups.map { g -> PttController.bareId(g.uri) to g }

        PullToRefreshBox(
            isRefreshing = refreshing,
            onRefresh = {
                refreshing = true
                st.ctl?.loadGroups()
                // loadGroups 는 fire-and-forget — 목록 반영 시간을 짧게 두고 인디케이터 종료
                scope.launch { delay(700); refreshing = false }
            },
            modifier = Modifier.weight(1f),
        ) {
            LazyColumn(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (rows.isEmpty()) {
                    items(listOf("empty")) {
                        Box(Modifier.fillMaxWidth().padding(vertical = 48.dp), contentAlignment = Alignment.Center) {
                            Text("채널 목록이 없습니다 — 아래로 당겨 새로고침", color = Ct.TextFaint, fontSize = 13.sp)
                        }
                    }
                }
                items(rows, key = { it.first }) { (id, g) ->
                    ChannelRow(st, id, g, onClick = { onOpenChannel(id) }, onMessage = { onOpenThread(id) })
                }
            }
        }
    }
}

/** 채널 행 — 좌측 이름 첫 자 배지 + 이름(주채널 "주" 표기)/태그 + 우측 메시지 버튼(시안). */
@Composable
private fun ChannelRow(
    st: PttUiState,
    id: String,
    g: GroupSummary,
    onClick: () -> Unit,
    onMessage: () -> Unit,
) {
    val name = g.displayName ?: id
    val s = st.session(id)
    val doc = st.groupDocs[id]   // TS 24.481 그룹 문서 — 우선순위·영상 여부
    // 맨 앞 = 그룹 이름 첫 자(아바타). 색만 상태 반영: 긴급>참여>미참여.
    val (color, dim) = when {
        s?.emergency == true -> Ct.Red to Ct.RedDim
        s != null -> Ct.Mint to Ct.MintDim
        else -> Ct.TextFaint to Ct.GrayDim
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
        RoleBadge(name.take(1), color, dim)
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(name, color = Ct.Text, fontSize = 15.sp, fontWeight = FontWeight.Bold)
                if (s?.role == ChannelRole.PRIMARY) SquareBadge("주", Ct.Mint)
                if (s?.emergency == true) SquareBadge("긴급", Ct.Red)
            }
            Row(
                horizontalArrangement = Arrangement.spacedBy(5.dp),
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(top = 2.dp),
            ) {
                if (doc?.video == true) TagChip("영상", R.drawable.ic_video) else TagChip("음성", R.drawable.ic_voice)
                // TS 24.481 on-network-group-priority (클수록 높음)
                doc?.priority?.let { TagChip("P$it") }
                // 편성 인원(그룹 문서) — 채널의 정원
                TagChip("${g.memberCount ?: doc?.members?.size ?: "-"}명")
                // 현재 접속 인원 — 참여하지 않은 채널도 conference 구독으로 알 수 있다.
                //   NOTIFY 를 아직 못 받았으면(null) 표시하지 않는다(0 과 "모름"을 구분).
                st.onlineCount(id)?.let { TagChip("접속 $it", tint = if (it > 0) Ct.Mint else Ct.TextDim) }
                val sp = s?.speaker
                when {
                    sp != null -> {
                        // 발언 중 — 마이크 아이콘 + 발언자(그룹 문서 이름 우선, 없으면 번호)
                        val spId = PttController.bareId(sp.id)
                        val spName = if (sp.self) "나"
                            else doc?.members?.firstOrNull { PttController.bareId(it.uri) == spId }?.name
                                ?.takeIf { it.isNotBlank() } ?: PttController.fmtNumber(spId)
                        Row(
                            Modifier.clip(RoundedCornerShape(6.dp)).background(Ct.MintDim)
                                .padding(horizontal = 7.dp, vertical = 2.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(4.dp),
                        ) {
                            Icon(painterResource(R.drawable.ic_voice), contentDescription = "발언 중",
                                tint = Ct.Mint, modifier = Modifier.size(11.dp))
                            Text("$spName 발언 중", color = Ct.Mint, fontSize = 11.sp,
                                fontWeight = FontWeight.Bold)
                        }
                    }
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
