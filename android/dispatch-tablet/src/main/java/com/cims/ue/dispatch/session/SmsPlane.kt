// 전화 축 문자(SMS/LMS) — SIP MESSAGE text/plain 1:1
//   정본: volte_supplementary_services.md §4.3, android_dispatch_tablet.md §6.2e
//
// **PTT 채널의 대화(MCData SDS)와 다른 것이다.** 이쪽은 전화 가족(volte∪voip) 계정으로 나가는 평문
// SIP MESSAGE 이고, 데스크톱도 같은 길을 쓴다(`DispatchSession.SendSms` → `SendRequest("MESSAGE", …)`).
//
// **외부망 번호는 게이트웨이가 있을 때만 보낸다.** 사이트에 외부망 SMS/LMS 게이트웨이가 연결돼 있는지는 프로파일이 말한다
// (`capabilities.smsGateway` — `ServiceProfile.smsGateway`). 없으면 보내면 서버가 거절하는데, 그 거절이 «번호가 틀렸다» 인지
// «망이 없다» 인지 관제사는 알 수 없다 — 그래서 앱이 먼저 막고 이유를 말한다
// (데스크톱 `SendAllowed = (!t.IsExternal || SmsGateway) && CanSms` 와 같은 규칙).
package com.cims.ue.dispatch.session

import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.SipMessage

/** 이 사이트에서 문자를 쓸 수 있는가 — 전화 계정이 서 있어야 한다. */
val DispatchSession.canSms: Boolean get() = phoneAccount != null

/**
 * 외부망 휴대전화 SMS/LMS 게이트웨이가 연결돼 있는가(프로파일 `capabilities.smsGateway` — 서버가 IBCF→SMSC / SMPP 로 내보낸다).
 * 외부 번호의 [문자] 가 열리는 조건이다 — 등록 가입자끼리의 문자와는 무관하다.
 */
val DispatchSession.smsGateway: Boolean get() = profile.value?.phoneService?.smsGateway == true

/** 이 번호로 문자를 보낼 수 없는가 — 외부망인데 게이트웨이가 없을 때만 막는다(순수 판정은 [smsBlocked]). */
fun DispatchSession.isSmsBlocked(number: String): Boolean = smsBlocked(isExternalNumber(number), smsGateway)

internal fun smsBlocked(external: Boolean, gateway: Boolean): Boolean = external && !gateway

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
    // 주소록에 있으면 그 줄이 답한다 — 서버 가입자는 사이트 안, CSV 의 external 줄은 외부망(데스크톱 `IsExternal`).
    book.entries.firstOrNull { DirectoryBook.normalize(it.msisdn) == n }?.let { return it.external }
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
    if (isSmsBlocked(peer)) return CimsResult.fail(-1, "외부망 번호 — 게이트웨이가 없습니다")

    val uri = if (target.contains(':')) target else "sip:$peer@${phoneDomain()}"
    val gen = loginGeneration.value
    val (r, early) = sendTracked({ it }) { a.sendRequest("MESSAGE", uri, "text/plain", text) }
    if (gen != loginGeneration.value) return CimsResult.fail(-1, "로그아웃됨")   // 비운 대화에 말풍선을 세우지 않는다
    addOutgoingSms(smsKey(peer), text, r.value ?: 0L, failed = !r.ok, early = early)   // 곧바로 실패했으면 처음부터 실패 말풍선
    return report(TextArea.SMS, r)
}

/** 실패한 문자 재전송 — **같은 말풍선**이 새 token 을 받는다(데스크톱 `SmsMessagesViewModel.ResendCore`). */
suspend fun DispatchSession.resendSms(m: Message): CimsResult<Unit> {
    if (m.kind != MessageKind.SMS) return CimsResult.ok(Unit)
    val a = phoneAccount ?: return report(TextArea.SMS, CimsResult.fail(-1, "전화 계정 없음"))
    if (!beginResend(m)) return CimsResult.ok(Unit)                 // 이미 다시 보내는 중이거나 실패가 아니다
    val uri = "sip:${m.groupId}@${phoneDomain()}"
    val (r, early) = sendTracked({ it }) { a.sendRequest("MESSAGE", uri, "text/plain", m.text) }
    markResent(m, "", r.value ?: 0L, failed = !r.ok, early = early)
    return report(TextArea.SMS, if (r.ok) CimsResult.ok(Unit) else CimsResult.fail(r.code, r.reason))
}

/**
 * 수신 — **전화 회선**으로 온 `text/plain` 만 문자로 본다(데스크톱 `SmsMessagesViewModel` 과 같다). PTT 회선으로 온 것까지
 * 받으면 무전 쪽 평문이 전화 문자 대화에 선다. 그 밖(SDS·XML 통지)은 제 경로가 따로 있다.
 */
internal fun DispatchSession.applySipMessage(msg: SipMessage) {
    if (!msg.contentType.startsWith("text/plain", ignoreCase = true)) return
    val phone = phoneAccount ?: return
    if (msg.accountId != phone.id) return
    val peer = userPart(msg.fromUri)
    if (peer.isEmpty()) return
    addIncomingSms(smsKey(peer), msg.fromUri, msg.body)
}

/**
 * 문자 **대화 키** — 번호의 비교 정규형(`010…` 과 `+8210…` 이 한 대화). 원 번호를 키로 쓰면 `010…` 으로 보낸 것과 `+8210…` 으로
 * 온 답이 다른 대화가 된다(데스크톱은 기록 줄을 `Canonical` 로 묶는다). 정규형이 비면(URI 등) 원문.
 */
internal fun smsKey(number: String): String {
    val u = userPart(number)
    // 번호 모양일 때만 정규형으로 — 영숫자 신원(`noc2`)을 숫자만 남기면 다른 번호의 대화에 합쳐지고, 국제 접두(`00…`)는
    //   국내 표기로 읽으면 엉뚱한 번호가 된다. 그런 것은 원문이 키다(키가 곧 답장 주소다).
    if (u.isEmpty() || u.any { !(it.isDigit() || it in "+-(). ") } || u.startsWith("00")) return u
    return DirectoryBook.normalize(u).ifEmpty { u }
}

/** 발신 URI 의 도메인 — 전화 계정 프로파일에서. 없으면 서버가 Route 로 처리하도록 빈 값. */
private fun DispatchSession.phoneDomain(): String =
    profile.value?.phoneService?.domain.orEmpty()            // 등록한 전화 회선(유선 voip 우선)과 같은 서비스
