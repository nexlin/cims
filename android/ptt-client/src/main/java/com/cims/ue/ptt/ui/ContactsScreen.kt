package com.cims.ue.ptt.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.activity.compose.BackHandler
import com.cims.ue.core.account.SsoProvisioner
import com.cims.ue.core.contacts.CompanyContact
import com.cims.ue.core.contacts.CompanyDirectoryStore
import com.cims.ue.ptt.HwPtt
import com.cims.ue.ptt.GroupCallState
import com.cims.ue.ptt.PttController
import com.cims.ue.ptt.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * 연락처 탭 — 회사 전화번호부(CSC `/provisioning/directory?service=ptt`) 기반 1:1 진입점.
 *
 * volte-client 회사 연락처와 같은 동선: 검색 + 조직 섹션 목록 → 행 탭 = **상세 화면**
 * (아바타/이름 중앙 + [싱글]/[멀티]/[문자] 액션 + 정보 행). 통화가 시작되면
 * [PrivateCallOverlay] (AppRoot 전역)가 전화앱 스타일 통화 화면을 띄운다.
 * 캐시를 먼저 그리고 진입 시 ETag 동기화(304=다운로드 생략, 오프라인 허용).
 *
 * **애드혹 그룹통화**: 행 long-press = 다중 선택 모드 → 체크 토글 → 하단 [그룹통화 N] =
 * 즉석 임시 그룹 발신([PttController.startAdhocCall]) — 통화 화면은 [AdhocCallOverlay].
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun ContactsScreen(st: PttUiState, onOpenThread: (String) -> Unit) {
    val context = LocalContext.current
    val store = remember { CompanyDirectoryStore(context) }
    var dir by remember { mutableStateOf(store.load()) }
    var syncNote by remember { mutableStateOf("") }
    var query by remember { mutableStateOf("") }
    var detail by remember { mutableStateOf<CompanyContact?>(null) }
    // 애드혹 다중 선택 (bareId 집합) — 비어 있지 않으면 선택 모드
    var picked by remember { mutableStateOf<Set<String>>(emptySet()) }
    val selecting = picked.isNotEmpty()
    BackHandler(enabled = selecting) { picked = emptySet() }

    LaunchedEffect(Unit) {
        val res = withContext(Dispatchers.IO) {
            SsoProvisioner.fetchDirectory(context, store.etag().ifBlank { null }, service = "ptt")
        }
        when {
            res == null -> if (dir.members.isEmpty()) syncNote = "동기화 실패 — 네트워크/로그인 확인"
            res.changed && res.dir != null -> {
                store.replace(res.dir!!, res.etag, System.currentTimeMillis())
                dir = res.dir!!
            }
            else -> store.touchSynced(System.currentTimeMillis())
        }
    }

    val me = PttController.bareId(st.ctl?.mcpttId ?: "")
    val orgNames = remember(dir) { dir.orgs.associate { it.code to it.name } }

    // 상세 화면 (전화앱 스타일) — 목록 위에 전체 표시, 뒤로가기로 복귀
    detail?.let { m ->
        ContactDetailView(st, m, orgNames[m.orgCode] ?: m.orgCode,
            onBack = { detail = null }, onOpenThread = onOpenThread)
        return
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().padding(horizontal = 16.dp)) {
        Spacer(Modifier.height(10.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Column {
                Text(if (selecting) "그룹통화 대상 선택" else "연락처", color = Ct.Text,
                    fontSize = 16.sp, fontWeight = FontWeight.Bold)
                Text(if (selecting) "선택 ${picked.size}명" else "1:1 무전·문자 · 길게 눌러 그룹통화",
                    color = Ct.TextFaint, fontSize = 11.sp)
            }
            Spacer(Modifier.weight(1f))
            if (selecting) {
                Text("취소", color = Ct.TextDim, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.clip(RoundedCornerShape(50)).clickable { picked = emptySet() }
                        .padding(horizontal = 10.dp, vertical = 6.dp))
            } else {
                Text("총 ${dir.members.count { PttController.bareId(it.number) != me }}명",
                    color = Ct.TextDim, fontSize = 12.sp)
            }
        }
        Spacer(Modifier.height(8.dp))
        // 검색 (volte 와 동일 동선 — 이름/번호 부분 일치)
        Box(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                .padding(horizontal = 12.dp, vertical = 9.dp),
        ) {
            BasicTextField(query, { query = it }, singleLine = true,
                textStyle = TextStyle(color = Ct.Text, fontSize = 13.sp),
                cursorBrush = SolidColor(Ct.Mint))
            if (query.isEmpty()) Text("이름·번호 검색", color = Ct.TextFaint, fontSize = 13.sp)
        }
        Spacer(Modifier.height(8.dp))

        val q = query.trim()
        val visible = remember(dir, me, q) {
            dir.members.filter { PttController.bareId(it.number) != me }
                .filter { q.isBlank() || it.name.contains(q, true) || it.number.contains(q) }
        }
        if (visible.isEmpty()) {
            Spacer(Modifier.height(40.dp))
            Text(if (q.isBlank()) syncNote.ifBlank { "연락처가 없습니다" } else "검색 결과가 없습니다",
                color = Ct.TextFaint, fontSize = 13.sp,
                modifier = Modifier.fillMaxWidth(), textAlign = TextAlign.Center)
        }
        val orgSort = remember(dir) { dir.orgs.sortedBy { it.sort }.map { it.code } }
        val byOrg = remember(visible) { visible.groupBy { it.orgCode } }
        val sections = (orgSort.filter { byOrg.containsKey(it) } + byOrg.keys.filterNot { orgSort.contains(it) })
            .map { code -> (orgNames[code] ?: code.ifBlank { "미지정" }) to byOrg.getValue(code) }
        LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            sections.forEach { (orgName, members) ->
                item(key = "org:$orgName") {
                    Text(orgName, color = Ct.Mint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
                        modifier = Modifier.padding(top = 10.dp, bottom = 4.dp))
                }
                items(members, key = { "m:${it.number}" }) { m ->
                    val peer = PttController.bareId(m.number)
                    val isPicked = picked.contains(peer)
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp))
                            .background(if (isPicked) Ct.MintDim else Color.Transparent)
                            .combinedClickable(
                                onClick = {
                                    if (selecting) picked = if (isPicked) picked - peer else picked + peer
                                    else detail = m
                                },
                                onLongClick = { picked = picked + peer },   // 선택 모드 진입/추가
                            )
                            .padding(horizontal = 4.dp, vertical = 9.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                    ) {
                        if (selecting) {
                            Box(
                                Modifier.size(34.dp).clip(CircleShape)
                                    .background(if (isPicked) Ct.Mint else Ct.SurfaceHi),
                                contentAlignment = Alignment.Center,
                            ) {
                                Text(if (isPicked) "✓" else "", color = Ct.Bg,
                                    fontSize = 16.sp, fontWeight = FontWeight.Bold)
                            }
                        } else {
                            Avatar(m.name, 34.dp)
                        }
                        Column(Modifier.weight(1f)) {
                            Text(m.name.ifBlank { m.number }, color = Ct.Text, fontSize = 14.sp,
                                fontWeight = FontWeight.SemiBold)
                            Text(m.number, color = Ct.TextFaint, fontSize = 11.sp)
                        }
                        val ps = st.session(peer)
                        if (selecting) {
                            // 선택 모드 — 행 전체가 토글, 개별 액션 숨김
                        } else if (ps != null) {
                            PillBadge(if (ps.fullDuplex) "멀티" else "싱글", Ct.Mint)
                        } else {
                            RowAction("싱글", Ct.Mint) { st.ctl?.startPrivateCall(peer, fullDuplex = false) }
                            RowAction("멀티", Ct.Amber) { st.ctl?.startPrivateCall(peer, fullDuplex = true) }
                            RowAction("문자", Ct.TextDim) { onOpenThread(peer) }
                        }
                    }
                    HorizontalDivider(color = Ct.Border, thickness = 0.5.dp)
                }
            }
            item { Spacer(Modifier.height(12.dp)) }
        }
        // 애드혹 발신 바 — 선택 모드에서만. 임시 그룹을 만들어 선택 인원 전원 호출.
        if (selecting) {
            Text("그룹통화 (${picked.size}명)", color = Ct.Bg, fontSize = 15.sp,
                fontWeight = FontWeight.Bold, textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp)
                    .clip(RoundedCornerShape(50)).background(Ct.Mint)
                    .clickable {
                        st.ctl?.startAdhocCall(picked.toList())
                        picked = emptySet()
                    }
                    .padding(vertical = 13.dp))
            Spacer(Modifier.height(6.dp))
        }
    }
}

/** 연락처 상세 — 전화앱 스타일 (아바타/이름 중앙 + 싱글/멀티/문자 액션 + 정보 행). */
@Composable
private fun ContactDetailView(
    st: PttUiState, m: CompanyContact, orgName: String,
    onBack: () -> Unit, onOpenThread: (String) -> Unit,
) {
    val peer = PttController.bareId(m.number)
    BackHandler(onBack = onBack)
    Column(Modifier.fillMaxSize().statusBarsPadding().padding(horizontal = 16.dp)) {
        Spacer(Modifier.height(10.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier.size(36.dp).clip(RoundedCornerShape(10.dp)).background(Ct.SurfaceHi)
                    .clickable(onClick = onBack),
                contentAlignment = Alignment.Center,
            ) {
                Icon(painterResource(R.drawable.ic_back), contentDescription = "뒤로",
                    tint = Ct.Text, modifier = Modifier.size(18.dp))
            }
        }
        Spacer(Modifier.height(28.dp))
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            Avatar(m.name, 92.dp, fontSize = 34.sp)
            Spacer(Modifier.height(14.dp))
            Text(m.name.ifBlank { m.number }, color = Ct.Text, fontSize = 21.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(26.dp))
            // 액션 — 싱글(한 명씩)/멀티(동시 발화)/문자
            Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp),
                horizontalArrangement = Arrangement.SpaceEvenly) {
                RoundAction("싱글", Ct.Mint) { st.ctl?.startPrivateCall(peer, fullDuplex = false) }
                RoundAction("멀티", Ct.Amber) { st.ctl?.startPrivateCall(peer, fullDuplex = true) }
                RoundAction("문자", Ct.TextDim) { onOpenThread(peer) }
            }
        }
        Spacer(Modifier.height(30.dp))
        HorizontalDivider(color = Ct.Border, thickness = 0.5.dp)
        InfoRow("번호", m.number)
        HorizontalDivider(color = Ct.Border, thickness = 0.5.dp)
        if (orgName.isNotBlank()) {
            InfoRow("소속", orgName)
            HorizontalDivider(color = Ct.Border, thickness = 0.5.dp)
        }
    }
}

/** 1:1 통화 화면 — 활성 private 세션이 있는 동안 전체 오버레이 (AppRoot 에서 표시).
 *  아바타/이름/모드 + 발언 상태 + 화면 PTT(터치 단말) + 나가기. HW PTT 키는 우선 규칙으로 동작. */
@Composable
fun PrivateCallOverlay(st: PttUiState, s: GroupCallState) {
    val context = LocalContext.current
    val name = remember(s.groupId) {
        CompanyDirectoryStore(context).load().members
            .firstOrNull { PttController.bareId(it.number) == s.groupId }?.name ?: s.groupId
    }
    Box(Modifier.fillMaxSize().background(Ct.Bg)) {
        Column(
            Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(36.dp))
            PillBadge(if (s.fullDuplex) "1:1 멀티" else "1:1 싱글", Ct.Mint)
            Spacer(Modifier.height(22.dp))
            Avatar(name, 100.dp, fontSize = 38.sp)
            Spacer(Modifier.height(14.dp))
            Text(name, color = Ct.Text, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Text(s.groupId, color = Ct.TextFaint, fontSize = 12.sp)
            Spacer(Modifier.height(18.dp))
            val talking = s.speaker
            Text(
                when {
                    !s.active && s.callId < 0 -> "연결 중…"
                    talking?.self == true -> "발언 중"
                    talking != null -> "상대 발언 중"
                    s.fullDuplex -> "PTT 를 누르고 말하세요 (동시 발화 가능)"
                    else -> "PTT 를 누르고 말하세요"
                },
                color = if (talking != null) Ct.Mint else Ct.TextDim, fontSize = 13.sp,
            )
            Spacer(Modifier.weight(1f))
            // 화면 PTT 바 — 1:1 통화 화면에서는 HW 키 단말에도 함께 제공(둘 다 동작).
            PttBar(floor = s.floorState, enabled = true, listenOnly = false,
                queuePosition = s.queuePosition, modifier = Modifier.fillMaxWidth(),
                onDown = { st.ctl?.pttDown() }, onUp = { st.ctl?.pttUp() })
            Spacer(Modifier.height(14.dp))
            Text("나가기", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth(0.55f).clip(RoundedCornerShape(50)).background(Ct.Red)
                    .clickable { st.ctl?.leaveGroup(s.groupId) }
                    .padding(vertical = 12.dp))
            Spacer(Modifier.height(10.dp))
        }
    }
}

/** 애드혹(즉석 그룹) 통화 화면 — 임시 그룹 세션이 있는 동안 전체 오버레이 (AppRoot 에서 표시).
 *  편성 채널이 아니므로 이름 대신 참여 인원·발언 상태를 보여주고, 나가면 세션째 소멸한다. */
@Composable
fun AdhocCallOverlay(st: PttUiState, s: GroupCallState) {
    val context = LocalContext.current
    val dirNames = remember {
        CompanyDirectoryStore(context).load().members
            .associate { PttController.bareId(it.number) to it.name }
    }
    fun nameOf(id: String) = dirNames[id]?.takeIf { it.isNotBlank() } ?: id
    val others = s.participants.keys.filter { it != PttController.bareId(st.ctl?.mcpttId ?: "") }
    Box(Modifier.fillMaxSize().background(Ct.Bg)) {
        Column(
            Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(36.dp))
            PillBadge("즉석 그룹통화", Ct.Amber)
            Spacer(Modifier.height(22.dp))
            Avatar("+", 100.dp, fontSize = 38.sp)
            Spacer(Modifier.height(14.dp))
            Text("참여 ${s.participants.size}명", color = Ct.Text, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Text(others.joinToString(" · ") { nameOf(it) }.ifBlank { "연결 중…" },
                color = Ct.TextFaint, fontSize = 12.sp, textAlign = TextAlign.Center,
                modifier = Modifier.padding(horizontal = 12.dp))
            Spacer(Modifier.height(18.dp))
            val talking = s.speaker
            Text(
                when {
                    !s.active && s.callId < 0 -> "연결 중…"
                    talking?.self == true -> "발언 중"
                    talking != null -> "${nameOf(PttController.bareId(talking.id))} 발언 중"
                    else -> "PTT 를 누르고 말하세요"
                },
                color = if (talking != null) Ct.Mint else Ct.TextDim, fontSize = 13.sp,
            )
            Spacer(Modifier.weight(1f))
            PttBar(floor = s.floorState, enabled = s.canRequestFloor, listenOnly = !s.canRequestFloor,
                queuePosition = s.queuePosition, modifier = Modifier.fillMaxWidth(),
                onDown = { st.ctl?.pttDown() }, onUp = { st.ctl?.pttUp() })
            Spacer(Modifier.height(14.dp))
            Text("나가기", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth(0.55f).clip(RoundedCornerShape(50)).background(Ct.Red)
                    .clickable { st.ctl?.leaveGroup(s.groupId) }
                    .padding(vertical = 12.dp))
            Spacer(Modifier.height(10.dp))
        }
    }
}

@Composable
private fun Avatar(name: String, size: androidx.compose.ui.unit.Dp,
                   fontSize: androidx.compose.ui.unit.TextUnit = 14.sp) {
    Box(
        Modifier.size(size).clip(CircleShape).background(Ct.MintDim),
        contentAlignment = Alignment.Center,
    ) {
        Text(name.take(1).ifBlank { "?" }, color = Ct.Mint, fontSize = fontSize, fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun RowAction(label: String, color: Color, onClick: () -> Unit) {
    Text(label, color = color, fontSize = 11.sp, fontWeight = FontWeight.SemiBold,
        modifier = Modifier.clip(RoundedCornerShape(50)).background(color.copy(alpha = 0.14f))
            .clickable(onClick = onClick)
            .padding(horizontal = 8.dp, vertical = 4.dp))
}

@Composable
private fun RoundAction(label: String, color: Color, onClick: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Box(
            Modifier.size(58.dp).clip(CircleShape).background(color.copy(alpha = 0.16f))
                .clickable(onClick = onClick),
            contentAlignment = Alignment.Center,
        ) {
            Text(label.take(2), color = color, fontSize = 15.sp, fontWeight = FontWeight.Bold)
        }
        Text(label, color = Ct.TextDim, fontSize = 12.sp)
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 12.dp)) {
        Text(label, color = Ct.TextFaint, fontSize = 11.sp)
        Spacer(Modifier.height(2.dp))
        Text(value, color = Ct.Text, fontSize = 15.sp)
    }
}
