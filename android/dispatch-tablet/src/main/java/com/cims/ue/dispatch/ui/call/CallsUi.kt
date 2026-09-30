// [통화] 화면의 상태·조작 묶음 (android_dispatch_tablet.md §6.3)
//
// 이 화면은 하위 패널이 여덟이고 각자 제 상태를 구독하고 있었다. 그래서 **VM 없이는 한 조각도 그려 볼 수
// 없었다.** 상태를 [CallsUi] 하나, 조작을 [CallsActions] 하나로 묶어 껍데기가 채우게 하면 본문은 순수
// 컴포저블이 되고 Preview 가 선다(§6.3 «화면을 보며 고친다»).
//
// 묶음으로 받는 이유는 **인자 수** 다 — 낱개로 펴면 서른 개가 넘어 호출부가 읽히지 않는다.
// 전부 기본값이 있어 Preview 는 **바꿔 볼 것만** 지정한다.
package com.cims.ue.dispatch.ui.call

import com.cims.ue.dispatch.session.CallLogRow
import com.cims.ue.dispatch.session.DeskTally
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.ui.PersonEntry

/** [통화] 화면이 그리는 데 필요한 값 전부. */
data class CallsUi(
    val dialNumber: String = "",
    val book: DirectoryBook = DirectoryBook(),
    val members: List<MemberChip> = emptyList(),
    val queue: List<QueueItem> = emptyList(),
    val tally: DeskTally = DeskTally(),
    val deskFilter: String = DESK_ALL,
    val calls: List<CallCard> = emptyList(),
    val transferTarget: String = "",
    val log: List<CallLogRow> = emptyList(),
    val live: List<LiveCallRow> = emptyList(),
    val liveHint: String = "",
    val watchDiag: List<CallDeskViewModel.WatchRow> = emptyList(),
    /**
     * 감청이 상대에게 **숨겨지는가** — 서버가 준 역할 속성(`listen_visibility`)이지 화면의 선택이 아니다
     * ([dispatch_center.md](../../../../../../../../../docs/design/features/dispatch_center.md) §5.6).
     * 감청 중인 행에 적어 «지금 내 청취가 보이는 상태인지» 를 관제사가 알게 한다.
     */
    val listenHidden: Boolean = true,
    /**
     * 사람 축 필터의 표시 이름 — 비면 걸려 있지 않다. 종류 필터와 **직교**하므로 칩을 따로 든다
     * (사람 메뉴의 «통화 기록» 이 건다, §6.2f).
     */
    val personFilter: String = "",
    /** 호별 수신 음량 — 감청 상세의 음량 막대. 없으면 1.0. */
    val rxLevels: Map<Int, Float> = emptyMap(),
)

/**
 * [통화] 화면이 일으키는 조작 전부. 기본값은 전부 무동작 — Preview 는 누르지 않는다.
 *
 * `personAt` 만 값을 돌려준다(사람 메뉴가 그 사람의 회선 구성을 물어본다, §6.2f).
 */
data class CallsActions(
    val setDialNumber: (String) -> Unit = {},
    val dial: () -> Unit = {},
    /** ⑥ 머리 [이력에서 보기] — 끝난 통화의 날짜별 조회·녹취는 [이력] 화면(§6.11). */
    val openHistory: () -> Unit = {},
    /** ⑥ 머리 [CSV] 의 내용 — 필터와 무관하게 오늘 내역 전부(데스크톱과 같다). */
    val logCsv: () -> String = { "" },
    /** 입력칸을 거치지 않고 그 번호로 — 제안 행의 [발신]. */
    val dialTo: (String) -> Unit = {},
    /** 감청 leg 의 수신 음량(0~2). */
    val setRxLevel: (Int, Float) -> Unit = { _, _ -> },
    val pickup: (String) -> Unit = {},
    val setDeskFilter: (String) -> Unit = {},
    val personAt: (String) -> PersonEntry? = { null },
    val monitorMember: (MemberChip) -> Unit = {},
    val answer: (CallCard) -> Unit = {},
    /** 대기열 [응답] — 그 발신자의 내 착신 leg. */
    val answerQueue: (QueueItem) -> Unit = {},
    /** 착신 거절 — 486 Busy Here(착신 배너 [거절] 과 같다). */
    val reject: (CallCard) -> Unit = {},
    val hangup: (CallCard) -> Unit = {},
    val toggleHold: (CallCard) -> Unit = {},
    val toggleMute: (CallCard) -> Unit = {},
    val openDtmf: (CallCard) -> Unit = {},
    val closeDtmf: () -> Unit = {},
    val sendDtmf: (CallCard, String) -> Unit = { _, _ -> },
    val openTransfer: (CallCard) -> Unit = {},
    val closeTransfer: () -> Unit = {},
    val setTransferTarget: (String) -> Unit = {},
    val pickTransferTarget: (String) -> Unit = {},
    val transfer: (CallCard) -> Unit = {},
    val consult: (CallCard) -> Unit = {},
    val completeConsult: (CallCard) -> Unit = {},
    val cancelConsult: (CallCard) -> Unit = {},
    val monitorLive: (LiveCallRow) -> Unit = {},
    val stopMonitorLive: (LiveCallRow) -> Unit = {},
    val clearPersonFilter: () -> Unit = {},
)
