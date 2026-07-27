package com.cims.ue.core

/**
 * CIMS 앱군(스위트) 간 협조 채널 상수 — 서명(signature) 권한 [PERMISSION] 으로 보호되는
 * 명시적(setPackage) 브로드캐스트.
 *
 * 마이크 핸드오프: Android 12+ 는 일반 앱 두 개의 동시 캡처를 허용하지 않고 한쪽에 무음을
 * 배달하므로(concurrent capture arbitration), PTT 발언 동안 통화(volte) 앱이 캡처를 양보해야
 * PTT 마이크가 확정적으로 열린다. 재생은 믹스되므로 조율 대상이 아니다.
 */
object CimsSuite {
    /** 스위트 서명 권한 — 브로드캐스트 송수신 양쪽에 요구(동일 서명 앱만 통과). */
    const val PERMISSION = "com.cims.ue.permission.CIMS_SUITE"

    const val VOLTE_PACKAGE = "com.cims.ue.volte"

    /** PTT 발언 시도/시작 — 통화 앱은 마이크를 양보한다(재생 유지, 캡처만 해제). */
    const val ACTION_MIC_YIELD = "com.cims.ue.action.MIC_YIELD"

    /** PTT 발언 종료 — 통화 앱 마이크 복귀. */
    const val ACTION_MIC_RESUME = "com.cims.ue.action.MIC_RESUME"
}
