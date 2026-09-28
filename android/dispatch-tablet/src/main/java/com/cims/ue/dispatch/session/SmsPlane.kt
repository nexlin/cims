// 전화 축 문자(SMS/LMS) — SIP MESSAGE text/plain 1:1
//   정본: volte_supplementary_services.md §4.3, android_dispatch_tablet.md §6.2e
//
// **PTT 채널의 대화(MCData SDS)와 다른 것이다.** 이쪽은 전화 가족(volte∪voip) 계정으로 나가는 평문
// SIP MESSAGE 이고, 데스크톱도 같은 길을 쓴다(`DispatchSession.SendSms` → `SendRequest("MESSAGE", …)`).
//
// **외부망 번호는 보내지 않는다.** 게이트웨이가 아직 없다 — 보내면 서버가 거절하는데, 그 거절이
// «번호가 틀렸다» 인지 «망이 없다» 인지 관제사는 알 수 없다. 그래서 앱이 먼저 막고 이유를 말한다
// (데스크톱 `SendAllowed = !t.IsExternal && CanSms` 와 같은 규칙).
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.SipMessage

/** 이 사이트에서 문자를 쓸 수 있는가 — 전화 계정이 서 있어야 한다. */
val DispatchSession.canSms: Boolean get() = phoneAccount != null

/**
 * 외부망 번호인가 — 주소록에 없고 내선 길이도 아닌 것.
 *
 * 판정을 앱이 하는 이유: 서버가 «가입자인지» 를 알려 주는 통로가 이 경로엔 없다. 주소록(전화 가족 합산
 * 축, §6.2b)이 사이트 가입자의 목록이므로 그것을 기준으로 본다 — 틀리면 보수적으로 외부망이 된다.
 */
fun DispatchSession.isExternalNumber(number: String): Boolean =
    isExternalNumber(phoneBook.value, number)

/** 같은 판정의 순수 형태 — 시험 대상. 세션이 갖는 것은 주소록 하나뿐이라 그것만 받는다. */
internal fun isExternalNumber(book: DirectoryBook, number: String): Boolean {
    val n = DirectoryBook.normalize(userPart(number))
    if (n.isEmpty()) return true
    if (book.entries.any { DirectoryBook.normalize(it.msisdn) == n }) return false
    // 내선(6자리 이하)은 사이트 안이다 — 주소록을 아직 못 받았을 때의 폴백.
    return userPart(number).length > 6
}

/**
 * 문자 발신 — 최종 응답은 token 으로 `requestResult` 에서 맞춘다(발신 말풍선 상태).
 *
 * 말풍선은 **보내기 전에** 세운다. 응답을 기다렸다 세우면 느린 망에서 «눌렀는데 아무 일도 없는» 구간이
 * 생긴다 — SDS 와 같은 규약이다.
 */
suspend fun DispatchSession.sendSms(target: String, text: String): CimsResult<Long> {
    val a = phoneAccount ?: return CimsResult.fail(-1, "전화 계정 없음")
    val peer = userPart(target)
    if (peer.isEmpty() || text.isBlank()) return CimsResult.fail(-1, "받는 사람·내용 없음")
    if (isExternalNumber(peer)) return CimsResult.fail(-1, "외부망 번호 — 게이트웨이가 없습니다")

    val uri = if (target.contains(':')) target else "sip:$peer@${phoneDomain()}"
    val r = a.sendRequest("MESSAGE", uri, "text/plain", text)
    addOutgoingSms(peer, text, if (r.ok) r.value ?: 0L else 0L)
    return r
}

/** 수신 — `text/plain` 만 문자로 본다. 그 밖(SDS·XML 통지)은 제 경로가 따로 있다. */
internal fun DispatchSession.applySipMessage(msg: SipMessage) {
    if (!msg.contentType.startsWith("text/plain", ignoreCase = true)) return
    val peer = userPart(msg.fromUri)
    if (peer.isEmpty()) return
    addIncomingSms(peer, msg.fromUri, msg.body)
}

/** 발신 URI 의 도메인 — 전화 계정 프로파일에서. 없으면 서버가 Route 로 처리하도록 빈 값. */
private fun DispatchSession.phoneDomain(): String =
    profile.value?.services?.firstOrNull { it.kind == "voip" || it.kind == "volte" }?.domain.orEmpty()
