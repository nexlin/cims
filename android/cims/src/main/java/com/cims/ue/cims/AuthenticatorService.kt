package com.cims.ue.cims

import android.app.Service
import android.content.Intent
import android.os.IBinder
import com.cims.ue.core.account.CimsAuthenticator

/** AccountManager 인증자 호스팅 서비스 — 다른 앱의 getAuthToken 요청이 여기로 바인딩된다. */
class AuthenticatorService : Service() {
    private val authenticator by lazy { CimsAuthenticator(this) }
    override fun onBind(intent: Intent?): IBinder = authenticator.iBinder
}
