package com.cims.ue.core.config

import android.content.Context

/**
 * SIP 설정 영속화. 플랫폼 SharedPreferences 사용(추가 의존성 없음).
 *
 * ⚠️ 비밀번호가 현재 평문 저장된다(개발용). 운영 단계에서는
 *    EncryptedSharedPreferences / Android Keystore 로 교체할 것.
 */
class ConfigStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("sip_config", Context.MODE_PRIVATE)

    fun load(): SipAccountConfig = SipAccountConfig(
        serverHost = prefs.getString(K_HOST, "").orEmpty(),
        serverPort = prefs.getInt(K_PORT, 5060),
        transport = runCatching {
            SipAccountConfig.Transport.valueOf(prefs.getString(K_TRANSPORT, "UDP").orEmpty())
        }.getOrDefault(SipAccountConfig.Transport.UDP),
        domain = prefs.getString(K_DOMAIN, "").orEmpty(),
        msisdn = prefs.getString(K_MSISDN, "").orEmpty(),
        displayName = prefs.getString(K_NAME, "").orEmpty(),
        loginId = prefs.getString(K_LOGIN, "").orEmpty(),
        authId = prefs.getString(K_AUTH, "").orEmpty(),
        password = prefs.getString(K_PW, "").orEmpty(),
        expiresSec = prefs.getInt(K_EXPIRES, 3600),
    )

    fun save(c: SipAccountConfig) {
        prefs.edit().apply {
            putString(K_HOST, c.serverHost)
            putInt(K_PORT, c.serverPort)
            putString(K_TRANSPORT, c.transport.name)
            putString(K_DOMAIN, c.domain)
            putString(K_MSISDN, c.msisdn)
            putString(K_NAME, c.displayName)
            putString(K_LOGIN, c.loginId)
            putString(K_AUTH, c.authId)
            putString(K_PW, c.password)
            putInt(K_EXPIRES, c.expiresSec)
            apply()
        }
    }

    fun isProvisioned(): Boolean = load().isComplete()

    private companion object {
        const val K_HOST = "host"
        const val K_PORT = "port"
        const val K_TRANSPORT = "transport"
        const val K_DOMAIN = "domain"
        const val K_MSISDN = "msisdn"
        const val K_NAME = "name"
        const val K_LOGIN = "login"
        const val K_AUTH = "auth"
        const val K_PW = "pw"
        const val K_EXPIRES = "expires"
    }
}
