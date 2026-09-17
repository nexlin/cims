// dialog 구독 대상 정규화 (docs/design/features/android_dispatch_tablet.md §6.2c)
//
// 실기에서 재현된 결함의 회귀 방지: 관제 그룹원 47명 중 **대표번호 1건만** 구독되고 나머지 46건은
// NOTIFY 를 하나도 못 받았다. 원인은 주소 형식이었다 —
//   · CSC 는 구성원을 `tel:` URI 로, 대표번호는 스킴 없이 낸다(csc `_tel_uri` vs `"pilotId": r[2]`).
//   · 코어 `normalizeTarget` 은 `tel:` 을 그대로 두고 맨 번호만 `sip:<n>@<domain>` 으로 만든다.
//   · `tel:` 은 호스트가 없어 라우팅할 수 없는 Request-URI 라 SUBSCRIBE 가 나가지 못한다.
package com.cims.ue.dispatch

import com.cims.ue.dispatch.session.routableTarget
import org.junit.Assert.assertEquals
import org.junit.Test

class RoutableTargetTest {

    @Test fun `tel 스킴은 벗긴다 — 코어가 sip 으로 만들 수 있게`() {
        assertEquals("+821011112222", routableTarget("tel:+821011112222"))
        assertEquals("1002", routableTarget("tel:1002"))
    }

    @Test fun `대소문자를 가리지 않는다`() {
        assertEquals("+821011112222", routableTarget("TEL:+821011112222"))
        assertEquals("+821011112222", routableTarget("Tel:+821011112222"))
    }

    @Test fun `sip 주소는 그대로 — 도메인이 실려 있어 라우팅된다`() {
        assertEquals("sip:1001@cims.local", routableTarget("sip:1001@cims.local"))
        assertEquals("sips:1001@cims.local", routableTarget("sips:1001@cims.local"))
    }

    @Test fun `스킴 없는 번호는 그대로 — 코어가 도메인을 붙인다`() {
        assertEquals("+82210001000", routableTarget("+82210001000"))
        assertEquals("1002", routableTarget("1002"))
    }

    @Test fun `앞뒤 공백을 다듬는다`() {
        assertEquals("+821011112222", routableTarget("  tel:+821011112222  "))
        assertEquals("+821011112222", routableTarget("tel: +821011112222"))
    }

    @Test fun `빈 값은 빈 값`() {
        assertEquals("", routableTarget(""))
        assertEquals("", routableTarget("tel:"))
    }
}
