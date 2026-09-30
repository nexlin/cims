// 주소록 캐시·로컬 CSV — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2b)
//
// 노리는 것은 **주소록이 틀린 사람을 가리키는** 결함이다 — CSV 가 서버 가입자의 이름을 덮거나 외부망으로 바꾸는 것, 따옴표 안
// 쉼표에서 열이 밀리는 것, 캐시가 ETag 를 잃어 매번 전부 받는 것, 외부망 번호로 문자가 나가는 것.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.CsvContact
import com.cims.ue.dispatch.session.CsvKind
import com.cims.ue.dispatch.session.DirectoryBook
import com.cims.ue.dispatch.session.DirectoryCache
import com.cims.ue.dispatch.session.DirectoryCsv
import com.cims.ue.dispatch.session.DirectoryEntry
import com.cims.ue.dispatch.session.OrgNode
import com.cims.ue.dispatch.session.isExternalNumber
import com.cims.ue.dispatch.session.mergeBook
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DirectoryStoreTest {

    @Test fun `데스크톱 directory csv 를 그대로 읽는다 — 머리줄·주석·BOM·따옴표`() {
        val text = "﻿# 주석\nkind,number,name,tags\n" +
            "ext,+821310001001,관제1석 1001,member\n" +
            "external,02-120,\"교통상황실, 야간\",\n" +
            "ptt,+82500000001,PTT단말1\n" +
            "\n" +
            "ext,,번호없음\n"
        val rows = DirectoryCsv.parse(text)
        assertEquals(listOf(CsvKind.EXT, CsvKind.EXTERNAL, CsvKind.PTT), rows.map { it.kind })
        assertEquals("교통상황실, 야간", rows[1].name)
        assertEquals(listOf("member"), rows[0].tags)
    }

    @Test fun `같은 종류·같은 번호는 뒤의 줄이 이긴다 — 모르는 kind 는 외부망`() {
        val rows = DirectoryCsv.parse("ext,1003,옛이름\next,1003,새이름\nfax,0212345678,팩스")
        assertEquals(listOf("새이름", "팩스"), rows.map { it.name })
        assertEquals(CsvKind.EXTERNAL, rows[1].kind)
    }

    @Test fun `서버 이름이 앞서고 CSV 는 빈 곳만 채운다`() {
        val server = DirectoryBook(entries = listOf(
            DirectoryEntry("T1", "김순경", "01011112222"),
            DirectoryEntry("T1", "", "01033334444")))
        val csv = listOf(
            CsvContact(CsvKind.EXT, "+821011112222", "다른이름"),     // 서버에 이름이 있다 — 덮지 않는다
            CsvContact(CsvKind.EXT, "01033334444", "이순경"),          // 서버 이름이 비었다 — 채운다
            CsvContact(CsvKind.EXTERNAL, "02-120", "교통상황실"),      // CSV 에만 — 뒤에 붙는다(외부망)
            CsvContact(CsvKind.PTT, "+82500000001", "PTT단말1"))       // 전화 주소록에는 섞지 않는다
        val book = mergeBook(server, csv, setOf(CsvKind.EXT, CsvKind.EXTERNAL))
        assertEquals(listOf("김순경", "이순경", "교통상황실"), book.entries.map { it.name })
        assertEquals(listOf(false, false, true), book.entries.map { it.external })
        assertEquals("T1", book.entries[1].org)
    }

    @Test fun `CSV 가 서버 가입자를 외부망으로 바꾸지 못한다`() {
        val server = DirectoryBook(entries = listOf(DirectoryEntry("T1", "김순경", "01011112222")))
        val book = mergeBook(server, listOf(CsvContact(CsvKind.EXTERNAL, "01011112222", "")), setOf(CsvKind.EXTERNAL))
        assertFalse(isExternalNumber(book, "01011112222"))
    }

    @Test fun `CSV 의 외부망 번호는 외부망이다 — 문자를 막는다`() {
        val book = mergeBook(DirectoryBook(), listOf(CsvContact(CsvKind.EXTERNAL, "1588", "콜센터")), setOf(CsvKind.EXTERNAL))
        assertTrue("짧은 번호라도 CSV 가 외부망이라 하면 외부망", isExternalNumber(book, "1588"))
    }

    @Test fun `캐시는 조직·줄·ETag 를 잃지 않는다`() {
        val b = DirectoryBook(
            orgs = listOf(OrgNode("C", "CIMS"), OrgNode("T1", "팀01", "C", 2)),
            entries = listOf(DirectoryEntry("T1", "김\"순경\"", "01011112222")),
            etag = "\"v7\"")
        val back = DirectoryCache.decode(DirectoryCache.encode(mapOf("volte" to b, "ptt" to DirectoryBook(etag = "p1"))))
        assertEquals(b, back["volte"])
        assertEquals("p1", back["ptt"]?.etag)
    }

    @Test fun `손상된 캐시는 빈 것으로 읽는다`() {
        assertTrue(DirectoryCache.decode("{not json").isEmpty())
    }
}
