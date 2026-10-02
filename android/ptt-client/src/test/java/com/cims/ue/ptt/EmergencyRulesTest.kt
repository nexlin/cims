package com.cims.ue.ptt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** 긴급 대상·경보 결과 판정(EmergencyRules) — 기기 없이 도는 것. */
class EmergencyRulesTest {
    // TS 24.379 §6.2.8.1.8 1)a) · §12.1.1.1 4)a)i)A) — DedicatedGroup 이면 긴급·경보 대상은 그 그룹(통화 중인 그룹이 달라도)
    @Test fun `전용 긴급그룹이면 통화 중인 그룹과 무관하게 그 그룹`() {
        assertEquals("g-sos", EmergencyRules.targetGroup("DedicatedGroup", "g-sos", primaryGroup = "g001", selectedGroup = "g002"))
        assertEquals("g-sos", EmergencyRules.targetGroup("DedicatedGroup", "g-sos", null, null))
    }

    @Test fun `전용 긴급그룹 미지정이면 대상 없음`() {
        assertNull(EmergencyRules.targetGroup("DedicatedGroup", null, "g001", "g002"))
    }

    // §12.1.1.1 4)a)i)B) — UseCurrentlySelectedGroup 인데 선택한 그룹이 없으면 문서의 uri-entry
    @Test fun `선택 그룹 모드는 주채널 다음 선택 그룹 다음 문서 폴백`() {
        assertEquals("g001", EmergencyRules.targetGroup("UseCurrentlySelectedGroup", "g-fb", "g001", "g002"))
        assertEquals("g002", EmergencyRules.targetGroup("UseCurrentlySelectedGroup", "g-fb", null, "g002"))
        assertEquals("g-fb", EmergencyRules.targetGroup("UseCurrentlySelectedGroup", "g-fb", null, null))
        assertNull(EmergencyRules.targetGroup("UseCurrentlySelectedGroup", null, null, null))
    }

    @Test fun `프로파일을 받지 못했으면 현재 주채널`() {
        assertEquals("g001", EmergencyRules.targetGroup(null, null, "g001", null))
    }

    // §12.1.1.1 — 발령 MESSAGE 의 4xx·5xx·6xx = MEA 1(경보 없음) · §12.1.1.2 — 취소 실패 = 경보 유지
    @Test fun `경보 발령은 2xx 일 때만 선다`() {
        assertTrue(EmergencyRules.alertStandsAfter(activate = true, code = 200))
        for (code in listOf(403, 404, 408, 480, 500, 503, 603))
            assertFalse("$code", EmergencyRules.alertStandsAfter(activate = true, code = code))
    }

    @Test fun `경보 취소는 2xx 일 때만 내려간다`() {
        assertFalse(EmergencyRules.alertStandsAfter(activate = false, code = 200))
        for (code in listOf(403, 404, 408, 480, 500, 503, 603))
            assertTrue("$code", EmergencyRules.alertStandsAfter(activate = false, code = code))
    }
}
