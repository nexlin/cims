// 망 복귀 판정(`NetworkReturn`) — JVM, 기기 불필요 (android_dispatch_tablet.md §6.1)
//
// 노리는 것은 둘이다 — 망이 돌아왔는데 **재등록하지 않아** 착신이 사라지는 것, 그리고 등록 직후의 첫 알림·같은 망의
// 재알림마다 **쓸데없이 재등록**해 서버에 REGISTER 가 몰리는 것.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.NetworkReturn
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NetworkReturnTest {

    @Test fun `등록 직후 지금의 망 알림은 복귀가 아니다`() {
        assertFalse(NetworkReturn<String>().onAvailable("wifi"))
    }

    @Test fun `같은 망의 재알림은 복귀가 아니다`() {
        val n = NetworkReturn<String>()
        n.onAvailable("wifi")
        assertFalse(n.onAvailable("wifi"))
    }

    @Test fun `잃었다 돌아오면 재등록한다`() {
        val n = NetworkReturn<String>()
        n.onAvailable("wifi")
        n.onLost("wifi")
        assertTrue(n.onAvailable("wifi"))
        assertFalse("한 번 되돌렸으면 끝이다", n.onAvailable("wifi"))
    }

    @Test fun `기본 망이 바뀌면 재등록한다 — 늦게 온 옛 망 소실은 무시`() {
        val n = NetworkReturn<String>()
        n.onAvailable("wifi")
        assertTrue(n.onAvailable("lte"))            // Wi-Fi → LTE: 주소가 바뀌었다
        n.onLost("wifi")                             // 옛 망의 소실이 뒤늦게 온다 — 지금 망(LTE)은 멀쩡하다
        assertFalse(n.onAvailable("lte"))
    }

    @Test fun `망 없이 기동했으면 처음 서는 망이 복귀다`() {
        // 부팅 직후·음영 구역 — 그동안 등록이 실패했으니 곧바로 다시 건다(갱신 주기를 기다리지 않는다).
        val n = NetworkReturn<String>()
        n.start(null)
        assertTrue(n.onAvailable("wifi"))
    }

    @Test fun `기동 때 망이 있으면 그 망의 첫 알림은 복귀가 아니다`() {
        val n = NetworkReturn<String>()
        n.start("wifi")
        assertFalse(n.onAvailable("wifi"))
        assertTrue("기동 뒤 다른 망으로 바뀌면 복귀다", n.onAvailable("lte"))
    }
}
