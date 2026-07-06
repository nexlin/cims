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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.ChannelRole
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.R
import com.cims.ue.ptt.csc.GroupMember

/** 우선순위 → 배지 색(시안: 1=붉은, 2~3=민트, 그 외=회색). */
private fun prioColors(p: Int?): Pair<Color, Color> = when {
    p != null && p <= 1 -> Ct.Red to Ct.RedDim
    p != null && p <= 3 -> Ct.Mint to Ct.MintDim
    else -> Ct.Gray to Ct.GrayDim
}

/**
 * 채널 상세(시안 `채널선택화면-상세.png`) — 헤더(우선순위·유형·CH) + 역할 배지 3케이스 +
 * 채널 상태 카드 + 접속 중/오프라인 구성원(이름·역할·우선순위·번호, TS 24.481 그룹 문서) +
 * 하단 [주채널 설정]/[부채널 설정] 케이스별 버튼.
 *
 * 구성원 정보의 출처는 GMS 그룹 문서(TS 24.481)의 표준 필드만 사용한다:
 * entry uri(tel:번호)·display-name·participant-type(chair/participant)·user-priority.
 * 접속/발언 상태는 conference-info(RFC 4575)·floor(TS 24.380) 로 실시간 결합.
 */
@Composable
fun ChannelDetailScreen(
    st: PttUiState,
    groupId: String,
    onBack: () -> Unit,
    onOpenThread: (String) -> Unit,
) {
    val s = st.session(groupId)
    val doc = st.groupDocs[groupId]
    val name = doc?.displayName ?: st.groupName(groupId)
    val joined = s != null

    // 그룹 문서(멤버 명부) 조회 — ETag 캐시라 재진입 시 304
    LaunchedEffect(groupId) { st.ctl?.loadGroupDetail(groupId) }

    Column(Modifier.fillMaxSize().statusBarsPadding().padding(horizontal = 16.dp, vertical = 10.dp)) {
        // 상단바 — 뒤로 + (채널명 / 우선순위·유형·CH) + 역할 배지
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Box(
                Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                    .clickable(onClick = onBack),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_back), contentDescription = "뒤로",
                    tint = Ct.Text, modifier = Modifier.size(18.dp))
            }
            Column(Modifier.weight(1f)) {
                Text(name, color = Ct.Text, fontSize = 18.sp, fontWeight = FontWeight.Bold)
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    modifier = Modifier.padding(top = 4.dp),
                ) {
                    doc?.priority?.let { p ->
                        val (c, d) = prioColors(p)
                        RoleBadge("$p", c, d)
                    }
                    TagChip(if (doc?.video == true) "영상" else "음성")
                    if (doc?.sessionType == "chat") TagChip("채팅형")
                    Text("CH $groupId", color = Ct.TextFaint, fontSize = 11.sp, fontWeight = FontWeight.Medium)
                }
            }
            when {
                s?.emergency == true -> PillBadge("긴급", Ct.Red, filled = true)
                s?.role == ChannelRole.PRIMARY -> PillBadge("주채널", Ct.Mint, filled = true)
                s?.role == ChannelRole.SECONDARY -> PillBadge("부채널", Ct.Amber, filled = true)
                joined -> PillBadge("참여 중", Ct.Gray)
            }
        }
        Spacer(Modifier.height(12.dp))

        // 역할 배지 3케이스 배너(시안) — 주채널/부채널/일반
        val notice = when {
            s?.role == ChannelRole.PRIMARY -> "현재 주채널로 설정되어 있습니다." to Ct.Mint
            s?.role == ChannelRole.SECONDARY -> "현재 부채널로 설정되어 있습니다." to Ct.Amber
            joined -> "주채널 또는 부채널로 설정할 수 있습니다." to Ct.TextDim
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

        // 접속 상태 결합 — 그룹 문서 명부(번호 키) × conference-info 참가자
        val me = st.ctl?.mcpttId?.let { PttController.bareId(it) }
        val speakerId = s?.speaker?.let { PttController.bareId(it.id) }
        val members = doc?.members.orEmpty()
        val byPhone = members.associateBy { PttController.bareId(it.uri) }
        val onlineIds = s?.participants.orEmpty()
        val online = onlineIds.keys.sortedWith(
            compareBy({ byPhone[it]?.priority ?: Int.MAX_VALUE }, { it }))
        val offline = members.filter { PttController.bareId(it.uri) !in onlineIds }
            .sortedBy { it.priority ?: Int.MAX_VALUE }

        // 채널 상태 카드 — 구성원 N명 · 접속 중 M명
        SectionCard {
            SectionLabel("채널 상태")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    val total = members.size.takeIf { it > 0 }
                        ?: st.groups.firstOrNull { PttController.bareId(it.uri) == groupId }?.memberCount
                    Text(
                        buildString {
                            append("구성원 ${total ?: "-"}명")
                            if (joined) append(" · 접속 중 ${online.size}명")
                        },
                        color = Ct.Text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
                    )
                    s?.speaker?.let { sp ->
                        val spName = byPhone[PttController.bareId(sp.id)]?.name
                        Text(
                            (if (sp.self) "내가" else spName ?: PttController.bareId(sp.id)) + " 발언 중",
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

        // 구성원 목록 — 접속 중(우선순위순) / 오프라인
        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (joined && online.isNotEmpty()) {
                items(listOf("접속 중")) { SectionLabel("접속 중 (${online.size})") }
                items(online, key = { "on:$it" }) { pid ->
                    val m = byPhone[pid]
                    MemberRow(
                        member = m, phone = pid, isMe = pid == me,
                        speaking = pid == speakerId,
                        pending = onlineIds[pid].equals("pending", true),
                        online = true,
                    )
                }
            }
            if (offline.isNotEmpty()) {
                // 미참여 시엔 접속 여부를 알 수 없으므로(RFC 4575 미구독) 중립 "구성원" 명부로 표시
                items(listOf("오프라인")) {
                    SectionLabel(if (joined) "오프라인 (${offline.size})" else "구성원 (${offline.size})")
                }
                items(offline, key = { "off:${it.uri}" }) { m ->
                    MemberRow(
                        member = m, phone = PttController.bareId(m.uri),
                        isMe = PttController.bareId(m.uri) == me,
                        speaking = false, pending = false, online = if (joined) false else null,
                    )
                }
            }
            if (!joined && members.isEmpty()) {
                items(listOf("hint")) {
                    Box(Modifier.fillMaxWidth().padding(vertical = 32.dp), contentAlignment = Alignment.Center) {
                        Text("구성원 정보를 불러오는 중…", color = Ct.TextFaint, fontSize = 13.sp)
                    }
                }
            }
        }

        // 하단 액션 — 시안 케이스별: A 주채널=버튼 없음(나가기만) / B 부채널=[주채널 설정] / C 일반=2버튼
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
                s?.role == ChannelRole.SECONDARY -> {
                    MintButton("주채널 설정", Modifier.weight(1f)) { st.ctl?.setPrimary(groupId) }
                    GhostButton("나가기", color = Ct.Red) { st.ctl?.leaveGroup(groupId); onBack() }
                }
                else -> {
                    MintButton("주채널 설정", Modifier.weight(1f)) { st.ctl?.setPrimary(groupId) }
                    GhostButton("부채널 설정", Modifier.weight(1f), color = Ct.Amber) {
                        st.ctl?.toggleSecondary(groupId)
                    }
                    GhostButton("나가기", color = Ct.Red) { st.ctl?.leaveGroup(groupId); onBack() }
                }
            }
        }
        Spacer(Modifier.height(10.dp))
    }
}

/**
 * 구성원 행 — 아바타 + 이름(+P우선순위·의장 배지) + 번호, 우측 상태(송신 중/대기/오프라인).
 * [member] 가 null 이면 그룹 문서에 없는 참가자(임시 초대 등) — 번호만 표시.
 * [online] null = 접속 여부 미상(미참여 명부) — 흐림·상태 표시 없이 중립 렌더.
 */
@Composable
private fun MemberRow(
    member: GroupMember?,
    phone: String,
    isMe: Boolean,
    speaking: Boolean,
    pending: Boolean,
    online: Boolean?,
) {
    val dispName = member?.name?.takeIf { it != member.uri && it != phone } ?: phone
    val dim = online == false
    val nameColor = if (dim) Ct.TextFaint else Ct.Text

    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp))
            .background(if (speaking) Ct.MintDim else Ct.Surface)
            .padding(horizontal = 12.dp, vertical = 9.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        InitialAvatar(dispName, active = online == true && !pending)
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(
                    if (isMe) "$dispName (나)" else dispName,
                    color = nameColor, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
                )
                member?.priority?.let { p ->
                    val (c, _) = prioColors(p)
                    PillBadge("P$p", if (dim) Ct.TextFaint else c)
                }
                if (member?.role == "chair") PillBadge("의장", if (dim) Ct.TextFaint else Ct.Amber)
            }
            if (dispName != phone) {
                Text(phone, color = if (dim) Ct.TextFaint else Ct.TextDim, fontSize = 11.sp,
                    modifier = Modifier.padding(top = 2.dp))
            }
        }
        if (online == true) {
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
                fontSize = 11.sp, fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.width(2.dp))
            StatusDot(if (speaking) Ct.Mint else Ct.Gray, 6)
        } else if (online == false) {
            StatusDot(Ct.GrayDim, 6)
        }
        if (speaking) {
            Icon(painterResource(R.drawable.ic_level_meter), contentDescription = null,
                tint = Ct.Mint, modifier = Modifier.size(16.dp))
        }
    }
}
