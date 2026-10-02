// [이력] 화면이 읽는 와이어 — 이력 항목의 확장 필드(서비스 축·MCVideo 속성·발언 수 유무)와 녹취 메타
// (android_ue_provisioning.md §3-2 · §3-4)
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.HistoryKind
import com.cims.ue.dispatch.session.ManagementClient
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HistoryWireTest {

    @Test fun `무전 항목 — 서비스 축과 MCVideo 세션 속성`() {
        val page = ManagementClient.parseHistory(HistoryKind.PTT, JSONObject("""
            {"items":[
              {"id":"v1","time":"2026-09-08T15:08:20+09:00","kind":"ptt","event":"ptt.session.end","service":"mcvideo",
               "mcvideo":{"sessionType":"chat","maxTransmitters":2},"turnCount":2,"totalSpeechMs":95000},
              {"id":"v2","time":"2026-09-08T15:20:00+09:00","kind":"ptt","service":"mcvideo",
               "mcvideo":{"session_type":"prearranged","max_transmitters":3}},
              {"id":"p1","time":"2026-09-08T15:30:00+09:00","kind":"ptt","service":"ptt","turnCount":0},
              {"id":"old","time":"2026-09-08T15:40:00+09:00","kind":"ptt"}]}
        """.trimIndent()))
        val by = page.items.associateBy { it.id }

        val v1 = by.getValue("v1")
        assertTrue(v1.isMcVideo)
        assertEquals("chat", v1.mcvSessionType)
        assertEquals(2, v1.mcvMaxTransmitters)
        // 서버가 세션 인덱스의 객체를 그대로 실어도(snake_case) 읽는다
        val v2 = by.getValue("v2")
        assertEquals("prearranged", v2.mcvSessionType)
        assertEquals(3, v2.mcvMaxTransmitters)

        val p1 = by.getValue("p1")
        assertEquals("ptt", p1.service)
        assertFalse(p1.isMcVideo)
        // 서비스 축이 없는 서버 — 무전으로 읽는다
        val old = by.getValue("old")
        assertEquals("", old.service)
        assertFalse(old.isMcVideo)
        assertEquals("", old.mcvSessionType)
        assertEquals(0, old.mcvMaxTransmitters)
    }

    @Test fun `무전 항목 — 발언 수가 실려 왔는지를 따로 안다`() {
        val page = ManagementClient.parseHistory(HistoryKind.PTT, JSONObject("""
            {"items":[
              {"id":"zero","time":"2026-09-08T13:20:30+09:00","kind":"ptt","turnCount":0},
              {"id":"unknown","time":"2026-09-08T13:21:30+09:00","kind":"ptt"},
              {"id":"null","time":"2026-09-08T13:22:30+09:00","kind":"ptt","turnCount":null}]}
        """.trimIndent()))
        val by = page.items.associateBy { it.id }
        assertTrue("0 이 실려 왔다 = 발언 없음", by.getValue("zero").hasTurnCount)
        assertFalse("안 실려 왔다 = 모름(스캔 폴백)", by.getValue("unknown").hasTurnCount)
        assertFalse(by.getValue("null").hasTurnCount)
        assertEquals(0, by.getValue("unknown").turnCount)
    }

    @Test fun `통화 항목 — 영상 통화와 진행 중`() {
        val page = ManagementClient.parseHistory(HistoryKind.CALL, JSONObject("""
            {"items":[
              {"id":"c1","time":"2026-09-08T13:12:03+09:00","kind":"call","callType":"volte_video","state":"ended",
               "inviteTime":"2026-09-08T13:06:50+09:00","answerTime":"2026-09-08T13:06:58+09:00",
               "endTime":"2026-09-08T13:12:03+09:00","endReason":"normal","emergency":true},
              {"id":"c2","time":"2026-09-08T13:40:00+09:00","kind":"call","callType":"volte","state":"active"}]}
        """.trimIndent()))
        val by = page.items.associateBy { it.id }
        assertTrue(by.getValue("c1").isVideoCall)
        assertTrue(by.getValue("c1").emergency)
        assertFalse(by.getValue("c1").isLive)
        assertFalse(by.getValue("c2").isVideoCall)
        assertTrue(by.getValue("c2").isLive)
    }

    @Test fun `진행 중인 무전 세션은 이벤트 이름으로도 안다`() {
        val page = ManagementClient.parseHistory(HistoryKind.PTT, JSONObject("""
            {"items":[{"id":"s","time":"2026-09-08T11:40:00+09:00","kind":"ptt","event":"ptt.session.start"}]}
        """.trimIndent()))
        assertTrue(page.items.single().isLive)
    }

    @Test fun `녹취 메타 — 세션의 서비스와 트랙 종류`() {
        val r = ManagementClient.parseRecording(JSONObject("""
            {"id":"ptt/1/2026/09/08/15/S_2","service":"mcvideo","start_time":"2026-09-08T15:05:00+09:00",
             "segments":[
               {"seq":1,"type":"mcvideo","speaker_id":"1002","duration_ms":60000,"has_video":true,"status":"ready",
                "start_time":"2026-09-08T15:05:06+09:00",
                "tracks":[{"slot":0,"kind":"audio","has_video":true,"speakers":[{"id":"1002","offset_ms":0,"dur_ms":60000}]},
                          {"slot":0,"kind":"video","has_video":true,"speakers":[{"id":"1002","offset_ms":0,"dur_ms":60000}]}]},
               {"seq":2,"type":"mcvideo","speaker_id":"1003","duration_ms":35000,"status":"ready",
                "start_time":"2026-09-08T15:07:00+09:00",
                "tracks":[{"slot":0,"kind":"video","speakers":[{"id":"1003","offset_ms":0,"dur_ms":35000}]}]},
               {"seq":3,"type":"ptt","duration_ms":1000,"tracks":[{"slot":1,"speakers":[]}]}]}
        """.trimIndent()), "x")

        assertEquals("mcvideo", r.service)
        assertTrue(r.isMcVideo)
        assertEquals(listOf("audio", "video"), r.segments[0].tracks.map { it.kind })
        assertTrue(r.segments[0].showsVideo)
        assertTrue("세그먼트에 표식이 없어도 영상 트랙이 있으면 영상이 있다", r.segments[1].showsVideo)
        assertEquals("종류가 안 실린 트랙은 음성이다", "audio", r.segments[2].tracks.single().kind)
        assertFalse(r.segments[2].showsVideo)
    }

    @Test fun `녹취 메타 — 서비스가 없어도 세그먼트 종류로 영상 세션임을 안다`() {
        val video = ManagementClient.parseRecording(JSONObject("""{"segments":[{"seq":1,"type":"mcvideo"}]}"""), "id")
        assertEquals("", video.service)
        assertTrue(video.isMcVideo)
        val voice = ManagementClient.parseRecording(JSONObject("""{"segments":[{"seq":1,"type":"ptt"}]}"""), "id")
        assertFalse(voice.isMcVideo)
    }

    @Test fun `세그먼트 경로 — 단독 트랙과 재변환`() {
        assertEquals("/provisioning/recordings/ptt/1/S_1/segments/3/audio", ManagementClient.segmentPath("ptt/1/S_1", 3, null, false))
        assertEquals("/provisioning/recordings/ptt/1/S_1/segments/3/audio?slot=1",
            ManagementClient.segmentPath("ptt/1/S_1", 3, 1, false))
        assertEquals("/provisioning/recordings/ptt/1/S_1/segments/3/audio?slot=1&retry=1",
            ManagementClient.segmentPath("ptt/1/S_1", 3, 1, true))
        assertEquals("/provisioning/recordings/a%20b/segments/1/audio?retry=1", ManagementClient.segmentPath("a b", 1, null, true))
    }

    @Test fun `변환을 기다리는 동안의 문구 — 녹음 중과 변환 중을 가른다`() {
        assertEquals("녹음 진행 중 — 세그먼트가 닫히면 재생됩니다", ManagementClient.transcodeStatusText("""{"status":"recording"}"""))
        assertEquals("서버가 변환 중입니다…", ManagementClient.transcodeStatusText("""{"status":"transcoding"}"""))
        assertEquals("서버가 변환 중입니다…", ManagementClient.transcodeStatusText(""))
        assertEquals("본문이 JSON 이 아니어도 죽지 않는다", "서버가 변환 중입니다…", ManagementClient.transcodeStatusText("<html>"))
    }
}
