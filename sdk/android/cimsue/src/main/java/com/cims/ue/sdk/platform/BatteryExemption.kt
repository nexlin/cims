// Android 접점 — 배터리 최적화(Doze) 예외 요청.
//
// `:core` 의 com.cims.ue.core.power.BatteryExemption 과 같은 계약이다. 두 공용층(:core / :cimsue)이
// 서로 의존하지 않아 한 곳에 둘 수 없으므로 규약을 맞춰 둔다. 설명은 그쪽 주석을 본다.
package com.cims.ue.sdk.platform

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings

object BatteryExemption {

    @Volatile private var askedThisProcess = false

    fun isExempt(context: Context): Boolean {
        val pm = context.getSystemService(PowerManager::class.java) ?: return false
        return pm.isIgnoringBatteryOptimizations(context.packageName)
    }

    /** 예외가 아니면 시스템 동의 다이얼로그를 띄운다. 한 프로세스에서 한 번만. */
    fun requestOnce(context: Context): Boolean {
        if (askedThisProcess || isExempt(context)) return false
        askedThisProcess = true
        return runCatching {
            context.startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                    .setData(Uri.parse("package:${context.packageName}"))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            )
            true
        }.getOrDefault(false)
    }
}
