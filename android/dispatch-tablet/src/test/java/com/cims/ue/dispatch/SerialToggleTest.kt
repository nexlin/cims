// 음소거 토글의 줄(`SerialToggle`) — JVM, 엔진 불필요 (android_dispatch_tablet.md §6.2e)
//
// 노리는 것은 **연타가 합쳐지는** 결함이다. 화면 사본으로 뒤집으면 스냅샷이 오기 전의 두 누름이 같은 값 둘로
// 나가 «두 번 눌렀는데 한 번 누른 상태» 가 된다 — 전이중 개별 통화에서는 곧 «음소거를 풀었다고 믿는데 안 나가는» 상태다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.SerialToggle
import com.cims.ue.sdk.CimsResult
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.joinAll
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SerialToggleTest {

    @Test fun `연타는 합쳐지지 않는다 — 뒤 누름은 앞 명령이 끝난 뒤의 값을 뒤집는다`() = runBlocking {
        var muted = false                                  // 코어 스냅샷 자리
        val firstDone = CompletableDeferred<Unit>()
        val writes = mutableListOf<Boolean>()
        val write: suspend (Boolean) -> CimsResult<Unit> = { v ->
            writes += v
            if (writes.size == 1) firstDone.await()        // 첫 명령이 끝나기 전에 두 번째 누름이 들어온다
            muted = v
            CimsResult.ok(Unit)
        }
        val t = SerialToggle()
        val a = launch { t.toggle({ muted }, write) }
        val b = launch { t.toggle({ muted }, write) }
        yield()                                            // 둘 다 들어온 뒤에 첫 명령을 끝낸다
        firstDone.complete(Unit)
        joinAll(a, b)

        assertEquals(listOf(true, false), writes)
        assertFalse(muted)                                 // 두 번 눌렀으니 제자리
    }

    @Test fun `호가 없으면 쓰지 않는다`() = runBlocking {
        var wrote = false
        val r = SerialToggle().toggle({ null }) { wrote = true; CimsResult.ok(Unit) }
        assertFalse(r.ok)
        assertFalse(wrote)
    }

    @Test fun `한 번 누르면 코어 값을 뒤집는다`() = runBlocking {
        var muted = true
        SerialToggle().toggle({ muted }) { v -> muted = v; CimsResult.ok(Unit) }
        assertFalse(muted)
        SerialToggle().toggle({ muted }) { v -> muted = v; CimsResult.ok(Unit) }
        assertTrue(muted)
    }
}
