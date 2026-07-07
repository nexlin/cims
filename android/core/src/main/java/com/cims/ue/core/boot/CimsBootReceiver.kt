package com.cims.ue.core.boot

import android.accounts.AccountManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import com.cims.ue.core.account.CimsAccounts

/**
 * 부팅 자동 로그인 — 기기 부팅(BOOT_COMPLETED) 시 공유 계정이 있으면 등록유지 서비스를 시작한다.
 * 서비스는 계정의 refresh_token 으로 무인 토큰 획득 → 프로비저닝 → SIP REGISTER 를 수행.
 *
 * 각 앱이 자기 서비스로 [serviceIntent] 를 구현해 서브클래싱하고, 매니페스트에 BOOT_COMPLETED 리시버로 등록한다.
 * (계정이 없으면 — 최초 1회 수동 로그인 전 — 아무것도 하지 않는다.)
 */
abstract class CimsBootReceiver : BroadcastReceiver() {

    /** 부팅 시 시작할 (등록유지 Foreground) 서비스 Intent. */
    abstract fun serviceIntent(context: Context): Intent

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != "android.intent.action.QUICKBOOT_POWERON") return
        // 공유 로그인 세션(계정)이 있어야 무인 자동 로그인 가능.
        if (CimsAccounts.get(AccountManager.get(context)) == null) return
        val svc = serviceIntent(context).apply { putExtra("autostart", true) }
        ContextCompat.startForegroundService(context, svc)
    }
}
