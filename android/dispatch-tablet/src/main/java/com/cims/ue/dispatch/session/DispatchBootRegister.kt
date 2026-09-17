// 부팅 재등록 (docs/design/features/android_dispatch_tablet.md §5)
//
// 저장된 자격이 있을 때만 서비스를 띄운다 — 판정은 접점층이 하고 이 클래스는 대상만 정한다.
package com.cims.ue.dispatch.session

import android.content.Context
import android.content.Intent
import com.cims.ue.sdk.platform.BootRegister

class DispatchBootRegister : BootRegister() {
    override fun serviceIntent(context: Context): Intent = Intent(context, DispatchService::class.java)
}
