// 관리 평면 와이어 파서 — 서버 응답 → 앱 모형 (docs/design/features/android_dispatch_tablet.md §9 S1-UE-TABLET-UNIT)
//
// 여기서 지키는 것은 **계약**이다. 서버가 실어 주지 않는 필드는 기본값이어야 하고, 전환기 서버의
// 다른 이름(`directoryAdmin`·`pickup_group`·volte 버킷의 voip 항목)도 같은 뜻으로 읽혀야 한다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.LineKind
import com.cims.ue.dispatch.session.ManagementClient
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ManagementWireTest {

    @Test fun `관리 한 벌 — 범위·서비스·조직·구성원을 읽는다`() {
        val v = ManagementClient.parseAdminView(JSONObject("""
            {"scope":{"groupId":"g1","directoryWrite":"all","orgCode":"HQ"},
             "services":{"volte":[{"name":"volte-a"},{"name":"voip-a","kind":"voip"}],
                         "ptt":[{"name":"ptt-a","kind":"mcptt"}]},
             "orgs":[{"code":"HQ","name":"본부","sort":1},{"code":"T1","name":"팀01","parent":"HQ"}],
             "members":[{"userId":7,"name":"김순경","loginId":"kim","org":"T1","title":"경장",
                         "volte":{"msisdn":"+821011112222","imsi":"450051","serviceRef":"volte-a","sipTransport":"TLS"},
                         "ptt":{"msisdn":"1001","profile":{"allowCreateGroup":true,"allowAmbientListening":false},
                                "pickup_group":"pg1"}}]}
        """.trimIndent()), "W/\"e1\"")

        assertEquals("all", v.scope.directoryWrite)
        assertTrue(v.scope.canWrite)
        assertEquals("W/\"e1\"", v.etag)
        // 버킷이 kind 의 기본값이고, 항목이 제 kind 를 실으면 그것이 이긴다.
        assertEquals(listOf("volte-a"), v.servicesOf(LineKind.VOLTE).map { it.name })
        assertEquals(listOf("voip-a"), v.servicesOf(LineKind.VOIP).map { it.name })
        assertEquals(listOf("ptt-a"), v.servicesOf(LineKind.PTT).map { it.name })

        val m = v.members.single()
        assertEquals(7L, m.userId)
        assertEquals("+821011112222", m.volte?.msisdn)
        assertEquals("450051", m.volte?.imsi)
        assertNull(m.voip)                                   // 서버가 안 실었으면 null — 빈 회선이 아니다
        assertTrue(m.allowCreateGroup)
        assertFalse(m.allowAmbientListening)
        assertEquals("pg1", m.ptt?.pickupGroup)               // snake_case 대체 이름도 읽는다
    }

    @Test fun `전환기 서버의 directoryAdmin 도 관리 범위로 읽는다`() {
        val v = ManagementClient.parseAdminView(JSONObject("""{"scope":{"directoryAdmin":"own"}}"""), "")
        assertEquals("own", v.scope.directoryWrite)
        assertTrue(v.scope.canWrite)
    }

    @Test fun `범위가 none 이면 관리 화면이 잠긴다`() {
        val v = ManagementClient.parseAdminView(JSONObject("""{"scope":{"directoryWrite":"none"}}"""), "")
        assertFalse(v.scope.canWrite)
    }

    @Test fun `조직 경로와 하위 집합`() {
        val v = ManagementClient.parseAdminView(JSONObject("""
            {"orgs":[{"code":"C","name":"CIMS"},{"code":"H","name":"본부","parent":"C"},
                     {"code":"T","name":"팀01","parent":"H"}]}
        """.trimIndent()), "")
        assertEquals("CIMS › 본부 › 팀01", v.orgPath("T"))
        assertEquals(setOf("H", "T"), v.orgSubtree("H"))
        assertNull(v.orgSubtree(""))                          // 빈 코드 = 필터 없음
    }

    @Test fun `조직 상위 고리가 있어도 경로가 멈춘다`() {
        val v = ManagementClient.parseAdminView(JSONObject("""
            {"orgs":[{"code":"A","name":"a","parent":"B"},{"code":"B","name":"b","parent":"A"}]}
        """.trimIndent()), "")
        assertEquals("b › a", v.orgPath("A"))                 // 무한 루프가 아니라 한 바퀴에서 끝난다
    }

    @Test fun `이력 — 필수 필드 없는 항목은 버리고 시각 오름차순으로 준다`() {
        val page = ManagementClient.parseHistory(HistoryKind.CALL, JSONObject("""
            {"hours":{"09":2,"14":1},
             "next":"cur-1",
             "items":[{"id":"b","time":"2026-09-15T14:00:00+09:00","kind":"call","state":"ended",
                       "endReason":"no_answer","duration":0,"inviteTime":"2026-09-15T14:00:00+09:00"},
                      {"id":"a","time":"2026-09-15T09:30:00+09:00","kind":"call","hasRecording":true,
                       "recordingId":"call/1/2026/09/15/09/S_1"},
                      {"time":"2026-09-15T10:00:00+09:00"},
                      {"id":"c"}]}
        """.trimIndent()))

        assertEquals(listOf("a", "b"), page.items.map { it.id })   // id·time 없는 둘은 버려진다
        assertEquals("cur-1", page.next)
        assertEquals(mapOf("09" to 2, "14" to 1), page.hours)
        assertTrue(page.items[0].hasRecording)
        assertEquals("no_answer", page.items[1].endReason)
    }

    @Test fun `이력 — PTT 확장 필드`() {
        val page = ManagementClient.parseHistory(HistoryKind.PTT, JSONObject("""
            {"items":[{"id":"s1","time":"2026-09-15T09:00:00+09:00","kind":"ptt","sessionKind":"group",
                       "groupName":"순찰1","turnCount":12,"speakerCount":4,"maxConcurrent":2,
                       "floorControl":"on","floorPolicy":"dual","totalSpeechMs":65000,
                       "people":["tel:1001","tel:1002"]}]}
        """.trimIndent()))
        val e = page.items.single()
        assertEquals("group", e.sessionKind)
        assertEquals(12, e.turnCount)
        assertEquals(listOf("tel:1001", "tel:1002"), e.people)
        assertFalse(e.isFullDuplex)                            // floorControl=on 이면 반이중
    }

    @Test fun `PTT 세션 상세 — 참여자·이벤트·floor`() {
        val d = ManagementClient.parsePttDetail(JSONObject("""
            {"participants":[{"msisdn":"1001","role":"initiator","join_time":"2026-09-15T09:00:00+09:00"},
                             {"role":"member"}],
             "events":[{"ts":"2026-09-15T09:00:01+09:00","type":"member_join","member":"1002"}],
             "floor":[{"ts":"2026-09-15T09:00:05+09:00","op":"DENY","user":"1002","reason":"busy","qsize":1,"pos":1}],
             "hasRecording":true}
        """.trimIndent()), "rec/1")

        assertEquals(1, d.participants.size)                   // msisdn 없는 참여자는 버린다
        assertEquals("initiator", d.participants[0].role)
        assertEquals("member_join", d.events.single().type)
        assertEquals("busy", d.floor.single().reason)
        assertEquals(1, d.floor.single().pos)
        assertEquals("rec/1", d.recordingId)                   // 응답에 없으면 호출자 값
        assertTrue(d.hasRecording)
    }

    @Test fun `녹취 — 슬롯 트랙과 화자 구간`() {
        val r = ManagementClient.parseRecording(JSONObject("""
            {"id":"ptt/24/2026/09/15/09/S_1","group_id":"g1","start_time":"2026-09-15T09:00:00+09:00",
             "segments":[{"seq":1,"speaker_id":"1001","duration_ms":4000,"status":"ready",
                          "start_time":"2026-09-15T09:00:00+09:00",
                          "tracks":[{"slot":0,"speakers":[{"id":"1001","offset_ms":0,"dur_ms":4000}]},
                                    {"slot":1,"speakers":[{"id":"1002","offset_ms":1000,"dur_ms":900}]}]},
                         {"seq":2,"speaker_id":"1002","duration_ms":2000,"status":"failed",
                          "start_time":"2026-09-15T09:00:10+09:00"}]}
        """.trimIndent()), "x")

        assertEquals("ptt/24/2026/09/15/09/S_1", r.id)
        assertEquals(2, r.segments.size)
        assertEquals(2, r.segments[0].tracks.size)
        assertEquals(900, r.segments[0].tracks[1].speakers.single().durMs)
        assertFalse(r.segments[1].playable)                    // failed = [다시 변환] 대상
    }

    @Test fun `그룹 목록 — canManage 가 없으면 구 서버로 보고 전부 관리 가능`() {
        val gs = ManagementClient.parseGroups(JSONObject("""
            {"groups":[{"id":"g1","uri":"tel:g1","name":"순찰1","memberCount":4,"isMember":true},
                       {"id":"g2","uri":"tel:g2","name":"상황실","canManage":false,"inListenScope":true}]}
        """.trimIndent()))
        assertTrue(gs[0].canManage)
        assertEquals("멤버", gs[0].relation)
        assertFalse(gs[1].canManage)
        assertEquals("청취 범위", gs[1].relation)
    }

    @Test fun `전화번호부 — 번호 없는 줄은 버린다`() {
        val b = ManagementClient.parseDirectory(JSONObject("""
            {"orgs":[{"code":"T","name":"팀01"}],
             "entries":[{"org":"T","name":"김순경","msisdn":"01011112222"},{"name":"빈칸"}]}
        """.trimIndent()), "e")
        assertEquals(1, b.entries.size)
        assertEquals("김순경", b.nameOf("+821011112222"))       // 로컬 표기 ↔ E.164 가 한 사람
        assertEquals("", b.nameOf("01099999999"))
    }

    @Test fun `녹취 id 는 경로형이라 구분자를 살린다`() {
        assertEquals("ptt/24/2026/09/15/09/S_1",
            ManagementClient.encPath("ptt/24/2026/09/15/09/S_1"))
        // 공백은 `+` 가 아니라 %20 이어야 한다 — 경로 조각이다(RFC 3986).
        assertEquals("a%20b/c", ManagementClient.encPath("a b/c"))
    }

    @Test fun `인코딩 — unreserved 는 그대로, 나머지는 퍼센트`() {
        assertEquals("aZ0-._~", ManagementClient.enc("aZ0-._~"))
        assertEquals("%2F%3F%26%3D", ManagementClient.enc("/?&="))
        assertEquals("%ED%95%9C", ManagementClient.enc("한"))      // UTF-8 3바이트
    }

    @Test fun `시각 — 오프셋 있는 것과 없는 것 둘 다 읽는다`() {
        val withOffset = ManagementClient.timeMs(JSONObject("""{"t":"2026-09-15T09:00:00+09:00"}"""), "t")
        assertEquals(1789430400000L, withOffset)
        assertTrue(ManagementClient.timeMs(JSONObject("""{"t":"2026-09-15T09:00:00"}"""), "t") != null)
        assertNull(ManagementClient.timeMs(JSONObject("""{"t":""}"""), "t"))
        assertNull(ManagementClient.timeMs(JSONObject("""{"t":"어제"}"""), "t"))
    }
}
