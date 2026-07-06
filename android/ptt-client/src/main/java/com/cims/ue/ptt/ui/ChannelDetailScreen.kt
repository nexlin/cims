package com.cims.ue.ptt.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.ChannelRole
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.R
import com.cims.ue.ptt.csc.GroupMember

/**
 * 채널 상세(시안 `채널선택화면-상세.png`, `채널상세화면-주채널표시.png`) —
 * 헤더(우선순위·유형·CH) + 역할 배너(주채널/일반 알약 배지, 배너 터치=토글) +
 * 채널 상태 카드(참여 중이면 수신 음량 슬라이더 포함) +
 * 접속 중/오프라인 구성원(이름·역할·우선순위·번호, TS 24.481 그룹 문서) +
 * 하단 참여/나가기 토글 버튼.
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
                    modifier = Modifier.padding(top = 1.dp),
                ) {
                    TagChip(if (doc?.video == true) "영상" else "음성")
                    // TS 24.481 on-network-group-priority (클수록 높음)
                    doc?.priority?.let { TagChip("P$it") }
                    if (doc?.sessionType == "chat") TagChip("채팅형")
                    Text("CH $groupId", color = Ct.TextFaint, fontSize = 11.sp, fontWeight = FontWeight.Medium)
                }
            }
            if (s?.emergency == true) PillBadge("긴급", Ct.Red, filled = true)
        }
        Spacer(Modifier.height(12.dp))

        // 역할 배너(시안 `채널상세화면-주채널표시.png`) — 민트 외곽선 박스 + 알약 배지 + 안내.
        // 배너 터치 = 주채널↔일반 토글(일반→주채널은 미참여 시 참여부터), 안내문도 함께 전환.
        val isPrimary = s?.role == ChannelRole.PRIMARY
        val bannerColor = if (isPrimary) Ct.Mint else Ct.Gray
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp))
                .background(if (isPrimary) Ct.Mint.copy(alpha = 0.06f) else Ct.Surface)
                .border(1.dp, bannerColor.copy(alpha = 0.5f), RoundedCornerShape(14.dp))
                .clickable {
                    st.ctl?.let { c ->
                        if (isPrimary) c.clearPrimary(groupId)
                        else {
                            if (!joined) c.joinGroupCall(groupId)
                            c.setPrimary(groupId)
                        }
                    }
                }
                .padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            // 배지 — 고정폭 소형 사각(모서리만 살짝 라운딩), 주채널/일반 크기 동일.
            // includeFontPadding 제거로 글자에 딱 붙는 최소 상하 여백(시안).
            Text(
                if (isPrimary) "주채널" else "일반",
                color = if (isPrimary) Ct.OnMint else Ct.TextDim,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                style = TextStyle(
                    fontSize = 11.sp, lineHeight = 12.sp,
                    platformStyle = PlatformTextStyle(includeFontPadding = false),
                ),
                modifier = Modifier.width(46.dp)
                    .clip(RoundedCornerShape(5.dp))
                    .background(if (isPrimary) Ct.Mint else Ct.SurfaceHi)
                    .border(1.dp, if (isPrimary) Ct.Text.copy(alpha = 0.35f)
                                  else Ct.Gray.copy(alpha = 0.4f), RoundedCornerShape(5.dp))
                    .padding(vertical = 1.dp),
            )
            Text(
                when {
                    isPrimary -> "현재 주채널로 설정되어 있습니다."
                    joined -> "터치하면 주채널로 설정됩니다."
                    else -> "터치하면 참여와 함께 주채널로 설정됩니다."
                },
                color = if (isPrimary) Ct.Mint else Ct.TextDim,
                fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                modifier = Modifier.weight(1f),
            )
        }
        Spacer(Modifier.height(12.dp))

        // 접속 상태 결합 — 그룹 문서 명부(번호 키) × conference-info 참가자
        val me = st.ctl?.mcpttId?.let { PttController.bareId(it) }
        val speakerId = s?.speaker?.let { PttController.bareId(it.id) }
        val members = doc?.members.orEmpty()
        val byPhone = members.associateBy { PttController.bareId(it.uri) }
        val onlineIds = s?.participants.orEmpty()
        // user-priority 는 클수록 높음(TS 24.481) — 높은 우선순위부터, 미지정=최저
        val online = onlineIds.keys.sortedWith(
            compareByDescending<String> { byPhone[it]?.priority ?: -1 }.thenBy { it })
        val offline = members.filter { PttController.bareId(it.uri) !in onlineIds }
            .sortedByDescending { it.priority ?: -1 }

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
                            (if (sp.self) "내가" else spName ?: PttController.fmtNumber(PttController.bareId(sp.id))) + " 발언 중",
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
            // 채널별 수신 음량 — conference bridge 유입 레벨(0~2, 1=원음). 참여 중일 때만.
            if (s != null) {
                Spacer(Modifier.height(8.dp))
                var vol by remember(groupId) { mutableFloatStateOf(s.volume) }
                Row(verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Icon(painterResource(R.drawable.ic_level_meter), contentDescription = "수신 음량",
                        tint = Ct.Mint, modifier = Modifier.size(16.dp))
                    Slider(
                        value = vol, onValueChange = {
                            vol = it
                            st.ctl?.setChannelVolume(groupId, it)
                        },
                        valueRange = 0f..2f,
                        modifier = Modifier.weight(1f).height(24.dp),
                        colors = SliderDefaults.colors(
                            thumbColor = Ct.Mint, activeTrackColor = Ct.Mint,
                            inactiveTrackColor = Ct.SurfaceHi, activeTickColor = Color.Transparent,
                            inactiveTickColor = Color.Transparent,
                        ),
                    )
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

        // 하단 액션 — 참여/나가기 토글 단일 버튼(주채널 설정/해제는 상단 배너에서)
        Spacer(Modifier.height(10.dp))
        if (!joined) {
            MintButton("참여", Modifier.fillMaxWidth()) { st.ctl?.joinGroupCall(groupId) }
        } else {
            GhostButton("나가기", Modifier.fillMaxWidth(), color = Ct.Red) {
                st.ctl?.leaveGroup(groupId); onBack()
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
    val phoneDisp = PttController.fmtNumber(phone)   // +82… → 0… 로컬 표기(표시 전용)
    val dispName = member?.name?.takeIf { it != member.uri && it != phone } ?: phoneDisp
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
                // TS 24.481 user-priority (0~255 클수록 높음) — 값만 표기
                member?.priority?.let { p ->
                    PillBadge("P$p", if (dim) Ct.TextFaint else Ct.Gray)
                }
                if (member?.role == "chair") PillBadge("의장", if (dim) Ct.TextFaint else Ct.Amber)
            }
            if (dispName != phoneDisp) {
                Text(phoneDisp, color = if (dim) Ct.TextFaint else Ct.TextDim, fontSize = 11.sp,
                    lineHeight = 12.sp)
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
