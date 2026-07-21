package com.cims.ue.core.sip

/**
 * SIP 등록/통화 상태의 UI 모델. **pjsua2 비참조 순수 Kotlin** 이므로 PJSIP 통합(M1.0) 전에
 * 선투입 가능하다 (설계 §3.8). `SipController` 가 pjsua2 콜백을 이 모델로 매핑해 StateFlow 로 노출하고,
 * volte-client UI 는 이 모델만 본다 — pjsua2 타입이 UI 레이어로 새지 않게 하는 경계.
 */

/** REGISTER 등록 상태. */
sealed interface RegState {
    /** 미등록(초기). */
    data object Idle : RegState

    /** REGISTER 발신 후 응답 대기. */
    data object Registering : RegState

    /** 등록 성공. [code] = 최종 응답 코드(통상 200). */
    data class Registered(val code: Int) : RegState

    /** de-REGISTER(expires=0) 완료 또는 등록 해제. */
    data object Unregistered : RegState

    /** 등록 실패. [reason] = "코드 사유"(예: "403 Forbidden"). */
    data class Failed(val reason: String) : RegState
}

/** 1:1 호 상태. */
sealed interface CallState {
    /** 통화 없음(초기). */
    data object Null : CallState

    /** 발신 진행(CALLING/EARLY). */
    data class Outgoing(val id: Int, val remote: String) : CallState

    /** 착신 수신(INVITE 도착, 180 자동 응답 후 사용자 응답 대기). [video]=상대 SDP 에 m=video 포함.
     *  [mcptt]=multipart 에 mcptt-info 포함(MCPTT 그룹콜 — PTT 앱이 자동 수락).
     *  [emergency]=mcptt-info 에 emergency-ind=true(긴급 그룹콜 fan-out — 긴급 UI/톤). */
    data class Incoming(
        val id: Int,
        val remote: String,
        val video: Boolean = false,
        val mcptt: Boolean = false,
        val emergency: Boolean = false,
    ) : CallState

    /** 통화 활성(CONNECTING/CONFIRMED). */
    data class Active(val id: Int, val remote: String) : CallState

    /** 통화 종료. [code]/[reason] = 마지막 상태 코드/사유. */
    data class Disconnected(val id: Int, val code: Int, val reason: String) : CallState
}

/** 수신 문자(SIP MESSAGE, RFC 3428 page-mode). [fromUri] 는 SIP URI 원문. */
data class ImMessage(val fromUri: String, val contentType: String, val body: String)

/**
 * MCData MSRP 미디어평면 호(SDS over media plane, TS 24.282 §9.2.3) 이벤트.
 * 일반 통화 상태([CallState])와 **분리** — MSRP 호의 대상 URI(그룹)가 PTT 음성 세션과 같아
 * [CallState] 로 흘리면 그룹 세션의 callId 를 오염시킨다(bareId 충돌).
 */
sealed interface MsrpEvent {
    /** MSRP INVITE 발신(UAC) 개시 — [callId] 확보(타임아웃 시 hangup 대상). */
    data class Started(val callId: Int, val remote: String) : MsrpEvent

    /** 200 OK answer 의 서버 MSRP `a=path` 학습 — TCP 접속 대상. */
    data class PathReady(val callId: Int, val path: String) : MsrpEvent

    /** 서버발 MSRP 배포 INVITE 착신(UAS) — [inviteMsg]=INVITE 원문(wholeMsg;
     *  a=path·mcdata-info 는 상위 계층이 파싱). 벨소리/통화 UI 없이 격리 처리. */
    data class Incoming(val callId: Int, val remote: String, val inviteMsg: String) : MsrpEvent

    /** UAS 200 answer 송신 완료(CONNECTING/CONFIRMED) — TCP 접속 개시 시점. */
    data class Answered(val callId: Int) : MsrpEvent

    /** MSRP 호 종료 — 서버 BYE(정상 완료 신호) 또는 실패 응답. */
    data class Closed(val callId: Int, val code: Int, val reason: String) : MsrpEvent
}

/** SIP URI("\"이름\" <sip:번호@도메인>")에서 표시용 번호만 추출. 패턴이 없으면 원문 반환. */
fun extractSipNumber(remote: String): String {
    var s = remote.trim()
    if (s.contains("<") && s.contains(">")) s = s.substringAfter("<").substringBefore(">")
    s = s.removePrefix("sip:").removePrefix("sips:").removePrefix("tel:")
    s = s.substringBefore("@").substringBefore(";")
    return s.ifBlank { remote }
}

/** ITU 자릿수 규칙 E.164 국가코드 추정 — 프로비저닝 countryCode 미수신 시 내 msisdn 에서 유도.
 *  1(NANP)/7=1자리, 유효 2자리 셋, 그 외 3자리. */
fun countryCodeOf(msisdn: String): String? {
    val d = msisdn.trim().removePrefix("tel:").removePrefix("+").filter { it.isDigit() }
    if (d.length < 4) return null
    if (d[0] == '1' || d[0] == '7') return d.take(1)
    val two = d.take(2)
    val twoDigit = setOf(
        "20", "27", "30", "31", "32", "33", "34", "36", "39", "40", "41", "43", "44", "45",
        "46", "47", "48", "49", "51", "52", "53", "54", "55", "56", "57", "58", "60", "61",
        "62", "63", "64", "65", "66", "81", "82", "84", "86", "90", "91", "92", "93", "94",
        "95", "98",
    )
    return if (two in twoDigit) two else d.take(3)
}

/** 발신번호 E.164 정규화 — 키패드 로컬 표기("013…")를 "+{cc}13…" 로.
 *  가입자 정본(IMPU/조회 키)은 E.164 라 로컬 표기 그대로 보내면 서버가 404.
 *  "+" 시작=그대로, 숫자 로컬(0 시작)+국가코드 확보 시=변환, 그 외(단축번호 등)=그대로. */
fun toE164(dialed: String, countryCode: String): String {
    val d = dialed.trim().replace(Regex("[\\s-]"), "")
    if (d.isEmpty() || d.startsWith("+")) return d
    if (countryCode.isBlank() || !d.all { it.isDigit() }) return d
    return if (d.startsWith("0")) "+$countryCode${d.drop(1)}" else d
}
