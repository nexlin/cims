// SDS 전달 확인 회신 — JVM, 기기 불필요 (android_dispatch_tablet.md §6.2e, mcdata_messaging.md §3)
//
// 노리는 것은 **상대 화면이 «보냄» 에 멈추는** 결함이다. 전달 확인을 요청받고도 DELIVERED 를 되돌리지 않으면 발신자는
// 관제석이 받았는지 알 수 없다. 반대로 요청하지 않은 것·통지 자체에 통지를 되돌리면 통지가 오가며 쌓인다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.deliveryReplyTo
import com.cims.ue.sdk.SdsMessage
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SdsDeliveryTest {

    private fun msg(disposition: Int = 1, notification: Boolean = false, msgId: String = "m1",
                    from: String = "sip:1003@cims") = SdsMessage(
        accountId = 0, fromUri = from, groupUri = "sip:g1@cims", convId = "c1", msgId = msgId, timeSec = 0,
        dispositionReq = disposition, text = "도착", notification = notification, notifType = 0,
        fd = false, fileUrl = "", fileName = "", fileType = "", fileSize = 0)

    @Test fun `전달 확인을 요청했으면 발신자에게 되돌린다`() {
        assertEquals("1003", deliveryReplyTo(msg(disposition = 1)))
        assertEquals("1003", deliveryReplyTo(msg(disposition = 3)))    // delivery + read — 전달분만 되돌린다
    }

    @Test fun `요청하지 않았거나 읽음만 요청했으면 되돌리지 않는다`() {
        assertNull(deliveryReplyTo(msg(disposition = 0)))
        assertNull(deliveryReplyTo(msg(disposition = 2)))              // 읽음 통지는 보내지 않는다(최소 프로파일)
    }

    @Test fun `통지에는 통지를 되돌리지 않는다`() {
        assertNull(deliveryReplyTo(msg(disposition = 1, notification = true)))
    }

    @Test fun `대사할 식별자나 상대가 없으면 되돌리지 않는다`() {
        assertNull(deliveryReplyTo(msg(msgId = "")))
        assertNull(deliveryReplyTo(msg(from = "")))
    }
}
