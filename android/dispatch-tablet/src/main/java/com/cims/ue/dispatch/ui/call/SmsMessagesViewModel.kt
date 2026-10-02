// [통화] > «메시지» 상태 — 전화 축 문자(SMS/LMS)
//   정본: android_dispatch_tablet.md §6.2e, volte_supplementary_services.md §4.3
//
// PTT 채널의 대화(`PttMessagesViewModel`)와 **같은 모양·다른 망**이다. 스레드 목록 : 대화 두 열이고,
// 스레드 키는 **상대 번호**다(그룹이 아니다 — 1:1 뿐이다).
package com.cims.ue.dispatch.ui.call

import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.canSms
import com.cims.ue.dispatch.session.isExternalNumber
import com.cims.ue.dispatch.session.isSmsBlocked
import com.cims.ue.dispatch.session.sendSms
import com.cims.ue.dispatch.session.resendSms
import com.cims.ue.dispatch.session.userPart
import com.cims.ue.dispatch.ui.RecipientOption
import com.cims.ue.dispatch.ui.ScreenViewModel
import com.cims.ue.dispatch.ui.ptt.ThreadChip
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

/** SMS 한 통은 70자, 넘으면 LMS — 데스크톱 `SmsLimit` 과 같은 값이다. */
const val SMS_LIMIT = 70

class SmsMessagesViewModel(private val s: DispatchSession) : ScreenViewModel() {

    private val _peer = MutableStateFlow<String?>(null)
    /** 고른 대화(상대 번호). null = 고르기 전. */
    val peer: StateFlow<String?> = _peer.asStateFlow()

    /** 대화 목록 — 최근 순. 이름은 주소록이 준다(없으면 번호). */
    val threads: StateFlow<List<ThreadChip>> =
        combine(s.sms, s.phoneBook) { map, book -> smsThreads(map, book) }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    val thread: StateFlow<List<Message>> =
        combine(s.sms, _peer) { map, p -> if (p == null) emptyList() else map[p].orEmpty() }
            .stateIn(scope, SharingStarted.Eagerly, emptyList())

    val title: StateFlow<String> =
        combine(_peer, s.phoneBook) { p, book ->
            if (p == null) "" else book.nameOf(p).ifBlank { com.cims.ue.dispatch.session.localNumber(p) }
        }.stateIn(scope, SharingStarted.Eagerly, "")

    /** 이 사이트에서 문자를 쓸 수 있는가(전화 계정). */
    val available: Boolean get() = s.canSms

    /** 고른 상대가 외부망인가 — 머리의 «외부망» 라벨. */
    val selectedIsExternal: StateFlow<Boolean> =
        _peer.combine(s.phoneBook) { p, _ -> p != null && s.isExternalNumber(p) }
            .stateIn(scope, SharingStarted.Eagerly, false)

    /**
     * 고른 상대에게 보낼 수 없는가 — **외부망인데 게이트웨이가 없을 때만**(프로파일 `smsGateway`, 데스크톱 `SendAllowed`).
     * 프로파일이 편성 재조회로 바뀌면 따라온다.
     */
    val selectedBlocked: StateFlow<Boolean> =
        combine(_peer, s.phoneBook, s.profile) { p, _, _ -> p != null && s.isSmsBlocked(p) }
            .stateIn(scope, SharingStarted.Eagerly, false)

    /**
     * «새 대화» 후보 — 전화 주소록 전부.
     *
     * 이미 대화가 있는 사람도 뺴지 않는다. 뺴면 «이 사람에게 보내려는데 목록에 없다» 가 되어 이유를 찾게
     * 되는데, 골랐을 때 기존 대화가 열리는 것이 맞는 동작이다([pick] 이 같은 키를 연다).
     */
    val candidates: StateFlow<List<RecipientOption>> =
        s.phoneBook.map { book ->
            book.entries.filter { it.msisdn.isNotBlank() }.map { e ->
                RecipientOption(
                    key = e.msisdn,
                    title = e.name.ifBlank { com.cims.ue.dispatch.session.localNumber(e.msisdn) },
                    subtitle = listOf(com.cims.ue.dispatch.session.localNumber(e.msisdn), book.orgPath(e.org)).filter { it.isNotBlank() }
                        .joinToString(" · "))
            }
        }.stateIn(scope, SharingStarted.Eagerly, emptyList())

    fun pick(peer: String) {
        val n = com.cims.ue.dispatch.session.smsKey(peer)        // 대화 키 = 정규형(`010…`·`+8210…` 이 한 대화)
        if (n.isEmpty()) return
        _peer.value = n
        s.markSmsRead(n)
    }

    /** 문자 면이 지금 보이는가 — 셸이 알려 준다([PttMessagesViewModel.setVisible] 과 같은 규칙). */
    private val _visible = MutableStateFlow(false)
    fun setVisible(v: Boolean) { _visible.value = v }

    init {
        // 열어 둔 대화로 온 문자는 곧바로 읽음 — 면이 보이고 화면이 켜져 있을 때만
        scope.launch {
            combine(s.sms, _peer, _visible, com.cims.ue.dispatch.session.UiPresence.visible) { all, p, vis, ui ->
                p?.takeIf { vis && ui && all[it]?.any { m -> !m.read && !m.outgoing } == true }
            }.collect { p -> if (p != null) s.markSmsRead(p) }
        }
    }

    /** 사람 메뉴 «문자» — 대화가 없으면 빈 스레드로 연다. */
    fun openTo(number: String) = pick(number)

    /** 실패한 말풍선 다시 보내기 — 같은 말풍선이 갱신된다(`resendSms`). */
    fun resend(m: Message) { scope.launch { s.resendSms(m) } }

    fun send(text: String) {
        val p = _peer.value ?: return
        if (text.isBlank()) return
        scope.launch { s.sendSms(p, text) }
    }
}

/**
 * 보관 → 대화 목록 (순수 함수, 시험 대상).
 *
 * 최근 순으로 세운다 — 방금 온 것이 앞이어야 찾는다. 빈 스레드는 내지 않는다.
 */
internal fun smsThreads(map: Map<String, List<Message>>, book: DirectoryBook): List<ThreadChip> =
    map.mapNotNull { (peer, msgs) ->
        if (msgs.isEmpty()) return@mapNotNull null
        ThreadChip(
            key = peer,
            title = book.nameOf(peer).ifBlank { com.cims.ue.dispatch.session.localNumber(peer) },
            unread = msgs.count { !it.read && !it.outgoing },
            lastAtMs = msgs.maxOf { it.atMs })
    }.sortedByDescending { it.lastAtMs }

/** 글자 수 표시 — 70자까지 SMS, 넘으면 LMS(데스크톱 `CountText` 와 같은 문구). */
internal fun smsCountText(text: String): String =
    if (text.length > SMS_LIMIT) "${text.length}자 LMS" else "${text.length}/$SMS_LIMIT SMS"
