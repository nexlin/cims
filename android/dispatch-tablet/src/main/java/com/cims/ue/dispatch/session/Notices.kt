// 토스트 — 명령 실패의 사유 (android_dispatch_tablet.md §6.2a-2, dispatch_desktop_ui.md §3.2·§9)
//
// 누른 것이 안 되면 **왜 안 됐는지**가 보여야 한다. 발신·당겨받기·청취 같은 명령은 결과가 SIP 응답으로 늦게 오고,
// 그 응답은 화면 어디에도 자리가 없다 — 조용히 삼키면 관제사는 눌렀는데 아무 일도 없었다고 느낀다. 사전(§9)의 문장을
// 띄우고, 원문 코드는 ▸상세 에 둔다.
package com.cims.ue.dispatch.session

/** 토스트 등급 — 데스크톱 `ToastLevel` 과 같다. */
enum class NoticeLevel { INFO, WARN, ERROR }

data class Notice(
    val id: Long,
    val level: NoticeLevel,
    /** 사전 문장(§9). */
    val text: String,
    /** 원문 코드·사유 — ▸상세. 비면 상세가 없다. */
    val detail: String = "",
    val atMs: Long = System.currentTimeMillis(),
) {
    /** 오류는 손으로 닫는다 — 이유를 읽기 전에 사라지면 안 된다. 정보·경고는 [NoticeBoard.AUTO_CLOSE_MS] 뒤 닫힌다. */
    val autoClose: Boolean get() = level != NoticeLevel.ERROR
}

/** 토스트 목록의 규칙 — 최신 위, 최대 [MAX]. 순수 논리라 JVM 에서 시험한다(데스크톱 `Notifications`). */
internal object NoticeBoard {
    const val MAX = 6
    const val AUTO_CLOSE_MS = 6_000L

    fun push(list: List<Notice>, n: Notice): List<Notice> = (listOf(n) + list).take(MAX)

    fun dismiss(list: List<Notice>, id: Long): List<Notice> = list.filterNot { it.id == id }
}
