// MCData FD 파일 평면의 순수 규칙(`FileRules`·`sweepFiles`) — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2e)
//
// 노리는 것: 상한은 **읽기 전에** 판정한다(넘는 파일을 메모리에 올리지 않는다) · 받은 파일 이름은 보낸 쪽이 준 것이라 그대로
// 믿지 않는다(경로 문자·숨김 파일·상위 폴더) · 같은 이름이 있으면 덮어쓰지 않고 «(n)» · [받기] 는 받은 파일에만, 받는 동안은
// 서지 않는다(두 번 받지 않는다) · 재전송은 이미 올린 파일을 다시 올리지 않는다 · 보관에서 사라진 말풍선의 파일만 지운다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.FileRules
import com.cims.ue.dispatch.session.Message
import com.cims.ue.dispatch.session.SendState
import com.cims.ue.dispatch.session.sweepFiles
import com.cims.ue.dispatch.ui.ptt.threadChips
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class FilePlaneTest {

    @get:Rule val tmp = TemporaryFolder()

    private fun file(out: Boolean = false, name: String = "현장사진_01.jpg", url: String = "https://csc/mcdata/fd/0a1b",
                     local: String = "", note: String = "", from: String = "이순경", state: SendState = SendState.NONE) =
        Message(id = "f1", groupId = "g1", fromUri = "tel:1003", fromName = from, text = "", atMs = 10, outgoing = out,
                state = state, fileName = name, fileUrl = url, fileSize = 1_258_291, fileType = "image/jpeg",
                localPath = local, transferNote = note)

    // ── 크기 낱말 ──

    @Test fun `크기는 한 낱말 — B · KB · MB, 소수 한 자리(0 이면 뗀다)`() {
        assertEquals("", FileRules.sizeText(0))
        assertEquals("", FileRules.sizeText(-1))
        assertEquals("812 B", FileRules.sizeText(812))
        assertEquals("1 KB", FileRules.sizeText(1024))
        assertEquals("1.5 KB", FileRules.sizeText(1536))
        assertEquals("340 KB", FileRules.sizeText(348_160))
        assertEquals("1.2 MB", FileRules.sizeText(1_258_291))
        assertEquals("50 MB", FileRules.sizeText(FileRules.MAX_BYTES))
        assertEquals("1.2 MB", file().fileSizeText)
    }

    // ── 상한 ──

    @Test fun `상한 50 MB — 딱 맞으면 보내고 한 바이트라도 넘으면 막는다`() {
        assertEquals(50L * 1024 * 1024, FileRules.MAX_BYTES)
        assertFalse(FileRules.tooLarge(FileRules.MAX_BYTES))
        assertTrue(FileRules.tooLarge(FileRules.MAX_BYTES + 1))
        assertFalse("크기를 모르는(-1) 파일은 읽으면서 센다", FileRules.tooLarge(-1))
        assertEquals("파일이 너무 큽니다 — 최대 50 MB", FileRules.tooLargeText)
        assertEquals("보고서.pdf · 61.3 MB", FileRules.tooLargeDetail("보고서.pdf", 64_277_709))
    }

    // ── 이름 ──

    @Test fun `받은 이름은 그대로 믿지 않는다 — 경로 문자·머리의 점·빈 이름`() {
        assertEquals("a_b_c.txt", FileRules.safeName("a/b:c.txt"))
        assertEquals("_.._etc_passwd", FileRules.safeName("../../etc/passwd"))      // 상위 폴더로 나가지 못한다
        assertEquals("bashrc", FileRules.safeName(".bashrc"))                       // 숨김 파일이 되지 않는다
        assertEquals("보고서.pdf", FileRules.safeName("  보고서.pdf  "))
        assertEquals("file.bin", FileRules.safeName(""))
        assertEquals("file.bin", FileRules.safeName(" .. "))
        assertEquals("a_b.txt", FileRules.safeName("a\nb.txt"))
    }

    @Test fun `너무 긴 이름은 확장자를 남기고 줄인다`() {
        val long = "가".repeat(200) + ".pdf"                                         // 한글 200자 = 600 바이트
        val safe = FileRules.safeName(long)
        assertTrue(safe.endsWith(".pdf"))
        assertTrue(safe.toByteArray(Charsets.UTF_8).size <= 200)
        assertTrue(safe.startsWith("가"))
    }

    @Test fun `같은 이름이 있으면 덮어쓰지 않고 (n) 을 붙인다`() {
        assertEquals("현장.jpg", FileRules.uniqueName("현장.jpg") { false })
        val have = mutableSetOf("현장.jpg")
        assertEquals("현장 (1).jpg", FileRules.uniqueName("현장.jpg") { it in have })
        have += "현장 (1).jpg"
        assertEquals("현장 (2).jpg", FileRules.uniqueName("현장.jpg") { it in have })
        assertEquals("README (1)", FileRules.uniqueName("README") { it == "README" })   // 확장자 없음
        assertEquals("a.tar (1).gz", FileRules.uniqueName("a.tar.gz") { it == "a.tar.gz" })
        assertEquals("file (1).bin", FileRules.uniqueName("") { it == "file.bin" })       // 이름 없는 파일도 겹친다
    }

    @Test fun `폴더는 mcdata 아래 둘 — 받은 파일과 보낸 사본`() {
        val files = tmp.root
        assertEquals(File(files, "mcdata"), FileRules.root(files))
        assertEquals(File(files, "mcdata/received"), FileRules.receivedDir(files))
        assertEquals(File(files, "mcdata/sent"), FileRules.sentDir(files))
    }

    // ── 말풍선 ──

    @Test fun `받기는 아직 안 받은 수신 파일에만 선다`() {
        assertTrue(FileRules.canDownload(file(), hasLocalFile = false))
        assertFalse("받는 중에는 한 번 더 받지 않는다", FileRules.canDownload(file(note = FileRules.DOWNLOADING), false))
        assertFalse("받아 둔 파일은 [열기]", FileRules.canDownload(file(), hasLocalFile = true))
        assertFalse("보낸 파일은 받을 것이 없다", FileRules.canDownload(file(out = true), false))
        assertFalse("받을 주소가 없다", FileRules.canDownload(file(url = ""), false))
        val text = Message(id = "t", groupId = "g1", fromUri = "", fromName = "", text = "글", atMs = 0, outgoing = false)
        assertFalse("글 말풍선", FileRules.canDownload(text, false))
    }

    @Test fun `기기에 있는가는 경로가 아니라 파일로 본다`() {
        val f = tmp.newFile("photo.jpg")
        val m = file(local = f.absolutePath)
        assertTrue(m.hasLocalFile)
        assertFalse(m.canDownload)
        f.delete()                                                                  // 저장소 비움·보관 정리 뒤
        assertFalse(m.hasLocalFile)
        assertTrue("다시 받을 수 있다", m.canDownload)
        assertFalse(file().hasLocalFile)
    }

    @Test fun `재전송 — 못 올린 파일은 업로드부터, 올린 파일은 알림만`() {
        assertTrue(FileRules.needsUpload(file(out = true, url = "", state = SendState.FAILED)))
        assertFalse(FileRules.needsUpload(file(out = true, state = SendState.FAILED)))
    }

    @Test fun `파일 말풍선 판정 — 이름이나 주소가 있으면 파일, 진행 문구가 있으면 전송 중`() {
        assertTrue(file().isAttachment)
        assertTrue(file(name = "").isAttachment)                                    // 이름 없는 FD 알림
        assertTrue(file(out = true, url = "").isAttachment)                         // 아직 안 올라간 발신 파일
        assertFalse(file().isTransferring)
        assertTrue(file(note = FileRules.UPLOADING).isTransferring)
        assertEquals("올리는 중…", FileRules.UPLOADING)
        assertEquals("받는 중…", FileRules.DOWNLOADING)
    }

    @Test fun `대화 목록의 한 줄 — 파일뿐이면 파일 이름`() {
        assertEquals("파일 현장사진_01.jpg", file().preview)
        assertEquals("글이 먼저", file().copy(text = "글이 먼저").preview)
        val chips = threadChips(mapOf("g1" to listOf(file())), isGroup = { true }) { "순찰1" }
        assertEquals("이순경: 파일 현장사진_01.jpg", chips.single().last)
        val mine = threadChips(mapOf("1003" to listOf(file(out = true)))) { "이순경" }
        assertEquals("나: 파일 현장사진_01.jpg", mine.single().last)
    }

    // ── 폴더 정리 ──

    @Test fun `어느 말풍선도 가리키지 않는 파일만 지운다 — 방금 생긴 것은 둔다`() {
        val root = FileRules.root(tmp.root)
        val received = FileRules.receivedDir(tmp.root).apply { mkdirs() }
        val sent = FileRules.sentDir(tmp.root).apply { mkdirs() }
        val now = 10_000_000_000L
        val old = now - 3_600_000L
        val kept = File(received, "kept.jpg").apply { writeText("a"); setLastModified(old) }
        val orphan = File(received, "orphan.jpg").apply { writeText("b"); setLastModified(old) }
        val sentOrphan = File(sent, "old-copy.pdf").apply { writeText("c"); setLastModified(old) }
        val fresh = File(sent, "fresh.pdf").apply { writeText("d"); setLastModified(now - 1_000L) }

        assertEquals(2, sweepFiles(root, setOf(kept.absolutePath), nowMs = now))
        assertTrue(kept.exists())
        assertFalse(orphan.exists())
        assertFalse(sentOrphan.exists())
        assertTrue("사본을 쓰고 경로를 적기까지의 틈", fresh.exists())
    }

    @Test fun `폴더가 없으면 지울 것도 없다`() {
        assertEquals(0, sweepFiles(File(tmp.root, "missing"), emptySet()))
    }
}
