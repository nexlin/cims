// PTT 그룹 편집 폼의 절 — [PTT 그룹] 화면의 인라인 폼과 «새 PTT 그룹» 패널의 [고급 설정] 이 **같은 절**을 그린다
// (android_dispatch_tablet.md §6.12, dispatch_desktop_ui.md §4.7 «폼의 그룹 호·한도 칸» · §10.6 «서비스» 절)
//
// 절은 넷이다 — 기본 / 그룹 호 / 허용·한도 / 서비스. 칸의 요소·범위·기본값은 콘솔 «서비스 › PTT 그룹» 편집과 같고
// (TS 24.481 §7.2.2·§7.2.4.2), 값의 규칙(미기재 보존·범위 자르기)은 GroupForm.kt 가 갖는다. 여기는 [EditForm] 을 그리고
// 바뀐 칸을 [onChange] 로 돌려줄 뿐이다 — 두 자리가 다른 칸·다른 기본값을 갖지 않게 절을 한 벌만 둔다.
package com.cims.ue.dispatch.ui.groups

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.cims.ue.dispatch.ui.FilterPill
import com.cims.ue.dispatch.ui.Segmented
import com.cims.ue.dispatch.ui.Tokens
import com.cims.ue.dispatch.ui.Type

/** 폼의 한 칸을 바꾸는 통로 — `onChange { it.copy(name = v) }`. */
internal typealias FormChange = ((EditForm) -> EditForm) -> Unit

/** 세션 종류 알약 — 편성(prearranged) / 채팅(chat). 일제 통화는 그룹 종류가 아니라 호 속성이라 없다(TS 24.379 §4.12). */
@Composable
internal fun SessionTypePills(value: String, onPick: (String) -> Unit) {
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        listOf("prearranged" to "편성(prearranged)", "chat" to "채팅(chat)").forEach { (v, label) ->
            FilterPill(label, value == v, onClick = { onPick(v) })
        }
    }
}

/** 그룹 우선순위 · 최대 참가자 — «기본» 절의 끝줄이자 패널 [고급 설정] 의 첫 줄. */
@Composable
internal fun PriorityRow(f: EditForm, onChange: FormChange) {
    FieldRow {
        Labeled("그룹 우선순위(0~15)", Modifier.weight(1f)) {
            NumberField(f.priority, { v -> onChange { it.copy(priority = v) } })
        }
        Labeled("최대 참가자(0 = 무제한)", Modifier.weight(1f)) {
            NumberField(f.maxParticipants, { v -> onChange { it.copy(maxParticipants = v) } })
        }
    }
}

/** 기본 — 이름 · id(신규만 편집) · 세션 종류 · 우선순위 · 최대 참가자. */
@Composable
internal fun BasicSection(f: EditForm, onChange: FormChange) {
    val p = Tokens.palette
    FormCard("기본") {
        Labeled("그룹 이름") { FormField(f.name, { v -> onChange { it.copy(name = v) } }, hint = "예) 3번 게이트 대응") }
        // id 는 만든 뒤 바꿀 수 없다 — 그룹 URI 가 곧 식별자다(identifier_model: 동작은 불변 id, 표시는 이름).
        Labeled("그룹 id — XCAP 문서 이름(만든 뒤 바꿀 수 없음)") {
            Column {
                FormField(f.groupId, { v -> onChange { it.copy(groupId = v) } }, enabled = f.isNew, mono = true,
                    keyboard = KeyboardType.Ascii)
                Text(f.docUri, fontSize = Type.micro, color = p.muted, fontFamily = FontFamily.Monospace, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(top = 3.dp))
            }
        }
        Labeled("세션 종류") { SessionTypePills(f.sessionType) { v -> onChange { it.copy(sessionType = v) } } }
        PriorityRow(f, onChange)
    }
}

/**
 * 그룹 호(TS 24.481 §7.2.2) — 유지 시간 T4 · 최대 통화 시간 · 시작 최소 응답 · 필수 멤버 대기 TNG1 · 대기 만료 동작 ·
 * 참가자 정보 구독. 범위는 저장할 때 자른다(T4 0~3600 · 최대 통화 0~86400 · 최소 응답 0~65535 · TNG1 1~300).
 */
@Composable
internal fun GroupCallSection(f: EditForm, onChange: FormChange) {
    FormCard("그룹 호") {
        FieldRow {
            // on-network-hang-timer — 발언 없이 이 시간이 지나면 그룹 호를 해제한다. 편성 그룹의 값이다.
            Labeled("유지 시간(T4, 초)", Modifier.weight(1f)) {
                NumberField(f.hangTimerSec, { v -> onChange { it.copy(hangTimerSec = v) } })
            }
            // on-network-maximum-duration
            Labeled("최대 통화 시간(TNG3, 초)", Modifier.weight(1f)) {
                NumberField(f.maxDurationSec, { v -> onChange { it.copy(maxDurationSec = v) } })
            }
        }
        FieldRow {
            // on-network-minimum-number-to-start — 개시자에게 응답하기 전에 받아야 할 멤버 응답 수
            Labeled("시작 최소 응답(명)", Modifier.weight(1f)) {
                NumberField(f.minNumberToStart, { v -> onChange { it.copy(minNumberToStart = v) } })
            }
            // on-network-timeout-for-acknowledgement-of-required-members — 필수 멤버가 있을 때만 쓰인다
            Labeled("필수 멤버 대기(TNG1, 초)", Modifier.weight(1f)) {
                NumberField(f.ackTimeoutSec, { v -> onChange { it.copy(ackTimeoutSec = v) } })
            }
        }
        // on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members
        Labeled("필수 멤버 대기가 끝나면") {
            val actions = listOf(GroupDefaults.ACK_ABANDON, GroupDefaults.ACK_PROCEED)
            Segmented(listOf("통화 포기", "없이 진행"), actions.indexOf(f.ackAction).coerceAtLeast(0),
                onSelect = { i -> onChange { it.copy(ackAction = actions[i]) } },
                modifier = Modifier.fillMaxWidth(), strong = false)
        }
        // on-network-allow-conference-state — 끄면 멤버의 conference 구독이 403(관제사 청취 범위는 별도)
        FormSwitch("참가자 정보 구독 허용", f.allowConferenceState, { v -> onChange { it.copy(allowConferenceState = v) } })
        FormHint("유지 시간(T4)은 편성 그룹에서 발언 없이 그 시간이 지나면 그룹 호를 해제합니다 — 0 = 미사용. 최대 통화 시간(TNG3) 0 = 무제한.")
    }
}

/** 허용 · 한도 — 스위치 여섯(긴급 그룹콜·긴급 경보·SDS·FD·암호화·affiliation) + MCData 크기 한도 둘. */
@Composable
internal fun LimitsSection(f: EditForm, onChange: FormChange) {
    FormCard("허용 · 한도") {
        Column {
            FieldRow {
                FormSwitch("긴급 그룹콜", f.emergencyCall, { v -> onChange { it.copy(emergencyCall = v) } }, Modifier.weight(1f))
                FormSwitch("긴급 경보", f.emergencyAlert, { v -> onChange { it.copy(emergencyAlert = v) } }, Modifier.weight(1f))
            }
            FieldRow {
                FormSwitch("메시지(SDS)", f.allowSds, { v -> onChange { it.copy(allowSds = v) } }, Modifier.weight(1f))
                FormSwitch("파일 전송(FD)", f.allowFd, { v -> onChange { it.copy(allowFd = v) } }, Modifier.weight(1f))
            }
            FieldRow {
                FormSwitch("미디어 암호화", f.encryption, { v -> onChange { it.copy(encryption = v) } }, Modifier.weight(1f))
                // 참여 전에 affiliation 이 있어야 한다
                FormSwitch("affiliation 필요", f.requireAffiliation, { v -> onChange { it.copy(requireAffiliation = v) } },
                    Modifier.weight(1f))
            }
        }
        FieldRow {
            // mcdata-on-network-max-data-size-for-SDS — 그룹 메시지 최대 크기
            Labeled("메시지 최대(byte)", Modifier.weight(1f)) {
                NumberField(f.maxSdsSize, { v -> onChange { it.copy(maxSdsSize = v) } })
            }
            // mcdata-on-network-max-data-size-auto-recv — 파일 자동 다운로드 임계
            Labeled("자동 수신 최대(byte)", Modifier.weight(1f)) {
                NumberField(f.maxAutoRecv, { v -> onChange { it.copy(maxAutoRecv = v) } })
            }
        }
        FormHint("한도 0 = 무제한.")
    }
}

/**
 * 서비스 — 한 그룹 = 서비스 집합(TS 23.280 §3). MCPTT 음성은 늘 켜 있고(위 «그룹 호»·«허용·한도» 가 이 서비스의 속성),
 * MCVideo 영상은 **켜기만** 한다: 이미 켜진 그룹은 스위치가 잠기고(끄기는 운영 콘솔), 이번 편집에서 켠 것은 저장 전까지
 * 되돌릴 수 있다. 종단간 보호는 꺼짐으로 명시해 싣고 칸은 잠근다(mcvideo.md D7).
 */
@Composable
internal fun ServiceSection(f: EditForm, onChange: FormChange) {
    FormCard("서비스") {
        Column {
            FormSwitch("MCPTT 음성 — 늘 켬", checked = true, onChange = {}, enabled = false)
            FormSwitch("MCVideo 영상", f.mcVideo, { v -> onChange { it.copy(mcVideo = v) } }, enabled = !f.mcVideoLocked,
                note = if (f.mcVideoLocked) "끄기는 운영 콘솔에서 — 이 앱의 저장은 MCVideo 를 끄지 못합니다"
                       else "켜면 멤버가 같은 그룹에서 음성 무전과 따로 영상 호를 엽니다")
        }
        if (f.mcVideo) {
            // mcvideo-on-network-invite-members — chat = 원하는 사람이 들어온다(합류가 곧 affiliation), 편성 = 제휴한 멤버를 초대
            Labeled("호 방식") {
                val types = listOf(GroupDefaults.MCV_CHAT, GroupDefaults.MCV_PREARRANGED)
                Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                    Segmented(listOf("chat", "편성"), types.indexOf(f.mcVideoType).coerceAtLeast(0),
                        onSelect = { i -> onChange { it.copy(mcVideoType = types[i]) } },
                        modifier = Modifier.fillMaxWidth(), strong = false)
                    FormHint("chat = 원하는 사람이 합류 · 편성 = 제휴한 멤버를 초대")
                }
            }
            FieldRow {
                // mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members (1~16)
                Labeled("동시 송출 상한(1~16)", Modifier.weight(1f)) {
                    NumberField(f.mcVideoMaxTransmitters, { v -> onChange { it.copy(mcVideoMaxTransmitters = v) } })
                }
                // mcvideo-on-network-maximum-duration (TNG3, 0 = 무제한)
                Labeled("최대 통화 시간(TNG3, 초)", Modifier.weight(1f)) {
                    NumberField(f.mcVideoMaxDurationSec, { v -> onChange { it.copy(mcVideoMaxDurationSec = v) } })
                }
            }
            FieldRow {
                // on-network-reception-hang-timer (T5, TS 24.581 §11.1.3, 0~3600)
                Labeled("수신 유지 시간(T5, 초)", Modifier.weight(1f)) {
                    NumberField(f.mcVideoReceptionHangSec, { v -> onChange { it.copy(mcVideoReceptionHangSec = v) } })
                }
                // mcvideo-on-network-minimum-number-to-start
                Labeled("시작 최소 응답(명)", Modifier.weight(1f)) {
                    NumberField(f.mcVideoMinNumberToStart, { v -> onChange { it.copy(mcVideoMinNumberToStart = v) } })
                }
            }
            // mcvideo-on-network-group-priority — 비우면 미기재(가장 낮음)
            Labeled("그룹 우선순위(0~255)") {
                FormField(f.mcVideoGroupPriority,
                    { v -> onChange { it.copy(mcVideoGroupPriority = v.filter(Char::isDigit).take(3)) } },
                    hint = "비우면 미기재", mono = true, keyboard = KeyboardType.Number)
            }
            Column {
                // mcvideo-on-network-allow-conference-state (TS 24.281 §9.2.3)
                FormSwitch("참가자 정보 구독 허용", f.mcVideoAllowConferenceState,
                    { v -> onChange { it.copy(mcVideoAllowConferenceState = v) } })
                // mcvideo-protect-media · mcvideo-protect-transmission-control — 요소가 없으면 켜짐으로 읽히므로 꺼짐을 명시해 싣는다
                FormSwitch("종단간 보호(미디어·전송 제어)", checked = false, onChange = {}, enabled = false,
                    note = "종단간 보호는 아직 지원하지 않습니다")
            }
        }
    }
}
