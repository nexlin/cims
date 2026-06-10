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

    /** 착신 수신(INVITE 도착, 180 자동 응답 후 사용자 응답 대기). */
    data class Incoming(val id: Int, val remote: String) : CallState

    /** 통화 활성(CONNECTING/CONFIRMED). */
    data class Active(val id: Int, val remote: String) : CallState

    /** 통화 종료. [code]/[reason] = 마지막 상태 코드/사유. */
    data class Disconnected(val id: Int, val code: Int, val reason: String) : CallState
}
