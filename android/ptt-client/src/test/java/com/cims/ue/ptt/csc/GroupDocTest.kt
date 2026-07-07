package com.cims.ue.ptt.csc

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** TS 24.481 그룹 문서 파싱 검증 — 서버(csc get_group_xml) 출력 형태 기준. */
class GroupDocTest {

    private val serverXml = """<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:ocp="urn:oma:xml:xdm:common-policy"
  xmlns:oxe="urn:oma:xml:xdm:extensions"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">
  <list-service uri="tel:+g001">
    <display-name xml:lang="en-us">관제센터 &amp; 본선</display-name>
    <list>
      <entry uri="tel:+82571900001">
        <rl:display-name>박관제</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:participant-type>chair</mcpttgi:participant-type>
        <mcpttgi:user-priority>1</mcpttgi:user-priority>
      </entry>
      <entry uri="tel:+82571900002">
        <rl:display-name>김기관</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:participant-type>participant</mcpttgi:participant-type>
        <mcpttgi:user-priority>5</mcpttgi:user-priority>
      </entry>
    </list>
    <mcpttgi:session-type>prearranged</mcpttgi:session-type>
    <mcpttgi:mcptt-video>false</mcpttgi:mcptt-video>
    <mcpttgi:on-network-max-participant-count>10</mcpttgi:on-network-max-participant-count>
    <mcpttgi:on-network-group-priority>1</mcpttgi:on-network-group-priority>
  </list-service>
</group>"""

    @Test fun parseServerDoc() {
        val d = GroupDoc.parse("tel:+g001", serverXml, "\"abcd\"")
        assertEquals("관제센터 & 본선", d.displayName)      // 그룹명 + 엔티티 복원
        assertEquals(1, d.priority)                          // group-priority (user-priority 와 미혼동)
        assertFalse(d.video)
        assertEquals("prearranged", d.sessionType)
        assertEquals(10, d.maxParticipants)
        assertEquals("\"abcd\"", d.etag)

        assertEquals(2, d.members.size)
        val chair = d.members[0]
        assertEquals("tel:+82571900001", chair.uri)
        assertEquals("박관제", chair.name)
        assertEquals("chair", chair.role)
        assertEquals(1, chair.priority)
        val m2 = d.members[1]
        assertEquals("김기관", m2.name)
        assertEquals("participant", m2.role)
        assertEquals(5, m2.priority)
    }

    @Test fun parseMinimalEntry() {
        val xml = """<group><list-service uri="tel:+g002"><display-name>g2</display-name><list>
          <entry uri="tel:+8210"></entry></list></list-service></group>"""
        val d = GroupDoc.parse("tel:+g002", xml, null)
        assertEquals(1, d.members.size)
        assertNull(d.members[0].name)
        assertEquals("participant", d.members[0].role)       // 기본값
        assertNull(d.members[0].priority)
        assertNull(d.priority)
        assertTrue(d.sessionType == null)
    }
}
