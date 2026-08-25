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
        transports = decodeTransports(prefs.getString(K_TRANSPORTS, "").orEmpty()),
        domain = prefs.getString(K_DOMAIN, "").orEmpty(),
        msisdn = prefs.getString(K_MSISDN, "").orEmpty(),
        imsi = prefs.getString(K_IMSI, "").orEmpty(),
        displayName = prefs.getString(K_NAME, "").orEmpty(),
        loginId = prefs.getString(K_LOGIN, "").orEmpty(),
        authId = prefs.getString(K_AUTH, "").orEmpty(),
        sipHa1 = prefs.getString(K_HA1, "").orEmpty(),
        password = prefs.getString(K_PW, "").orEmpty(),
        authScheme = prefs.getString(K_AUTH_SCHEME, "digest").orEmpty().ifBlank { "digest" },
        akaK = prefs.getString(K_AKA_K, "").orEmpty(),
        akaOpc = prefs.getString(K_AKA_OPC, "").orEmpty(),
        akaAmf = prefs.getString(K_AKA_AMF, "8000").orEmpty().ifBlank { "8000" },
        secMechanisms = prefs.getString(K_SEC_MECH, "").orEmpty()
            .split(',').map { it.trim() }.filter { it.isNotBlank() },
        expiresSec = prefs.getInt(K_EXPIRES, 3600),
        countryCode = prefs.getString(K_CC, "").orEmpty(),
        maxPayloadSdsCplaneBytes = prefs.getInt(K_SDS_CPLANE_MAX, 0),
    )

    fun save(c: SipAccountConfig) {
        prefs.edit().apply {
            putString(K_HOST, c.serverHost)
            putInt(K_PORT, c.serverPort)
            putString(K_TRANSPORT, c.transport.name)
            putString(K_TRANSPORTS, encodeTransports(c.transports))
            putString(K_DOMAIN, c.domain)
            putString(K_MSISDN, c.msisdn)
            putString(K_IMSI, c.imsi)
            putString(K_NAME, c.displayName)
            putString(K_LOGIN, c.loginId)
            putString(K_AUTH, c.authId)
            putString(K_HA1, c.sipHa1)
            putString(K_PW, c.password)
            putString(K_AUTH_SCHEME, c.authScheme)
            putString(K_AKA_K, c.akaK)
            putString(K_AKA_OPC, c.akaOpc)
            putString(K_AKA_AMF, c.akaAmf)
            putString(K_SEC_MECH, c.secMechanisms.joinToString(","))
            putInt(K_EXPIRES, c.expiresSec)
            putString(K_CC, c.countryCode)
            putInt(K_SDS_CPLANE_MAX, c.maxPayloadSdsCplaneBytes)
            apply()
        }
    }

    fun isProvisioned(): Boolean = load().isComplete()

    /**
     * 프로비저닝 결과 저장 — **사용자가 고른 transport 는 유지**한다. 서버가 주는 transport 는
     * 강제가 아니라 기본값(권장)이고 선택권은 단말에 있다(sip_tls_signaling.md §7.1).
     * 선택값이 새 가용 목록에서 사라졌으면(서버가 그 transport 를 내렸다) 서버 기본값으로 강등하고
     * 선택 표시도 지운다 — 도달 불가한 경로를 붙들고 있지 않기 위함.
     * 사용자가 고른 적이 없으면 서버 기본값을 그대로 따른다.
     */
    fun saveProvisioned(fresh: SipAccountConfig) {
        if (isTransportUserSet()) {
            val chosen = load().transport
            if (fresh.transports.any { it.transport == chosen }) {
                save(fresh.withTransport(chosen))
                return
            }
            setTransportUserSet(false)
        }
        save(fresh)
    }

    /** 사용자가 설정 화면에서 고른 transport 저장 — 이후 프로비저닝 재취득에도 유지된다. */
    fun saveTransportChoice(t: SipAccountConfig.Transport) {
        save(load().withTransport(t))
        setTransportUserSet(true)
    }

    /** 사용자가 transport 를 직접 고른 적이 있는가(= 서버 기본값보다 우선). */
    fun isTransportUserSet(): Boolean = prefs.getBoolean(K_TRANSPORT_USER, false)

    private fun setTransportUserSet(on: Boolean) {
        prefs.edit().putBoolean(K_TRANSPORT_USER, on).apply()
    }

    /** 가용 목록 직렬화 — "UDP:15060,TCP:15060,TLS:15061" (SharedPreferences 에 목록형이 없다). */
    private fun encodeTransports(l: List<SipAccountConfig.TransportEndpoint>): String =
        l.joinToString(",") { "${it.transport.name}:${it.port}" }

    private fun decodeTransports(s: String): List<SipAccountConfig.TransportEndpoint> =
        s.split(',').mapNotNull { e ->
            val kv = e.split(':')
            if (kv.size != 2) return@mapNotNull null
            val t = runCatching {
                SipAccountConfig.Transport.valueOf(kv[0].trim().uppercase())
            }.getOrNull() ?: return@mapNotNull null
            val port = kv[1].trim().toIntOrNull()?.takeIf { it in 1..65535 } ?: return@mapNotNull null
            SipAccountConfig.TransportEndpoint(t, port)
        }

    /** 로그아웃 — 프로비저닝된 계정/서버 설정 전부 제거(캐시 자격증명으로 재등록되지 않게). */
    fun clear() {
        prefs.edit().clear().apply()
    }

    /**
     * 수동 설정 모드 — true 면 SSO 자동 프로비저닝이 저장값을 덮어쓰지 않는다.
     * CIMS 로그인 없이(또는 무시하고) 직접 입력한 값을 시험하기 위한 모드. 끄면 다음
     * 프로비저닝 시 서버 값으로 복원된다.
     */
    fun isManual(): Boolean = prefs.getBoolean(K_MANUAL, false)

    fun setManual(on: Boolean) {
        prefs.edit().putBoolean(K_MANUAL, on).apply()
    }

    private companion object {
        const val K_HOST = "host"
        const val K_PORT = "port"
        const val K_TRANSPORT = "transport"
        const val K_TRANSPORTS = "transports"          // 서버 알림 가용 목록
        const val K_TRANSPORT_USER = "transport_user"  // 사용자 직접 선택 표시
        const val K_DOMAIN = "domain"
        const val K_MSISDN = "msisdn"
        const val K_IMSI = "imsi"
        const val K_NAME = "name"
        const val K_LOGIN = "login"
        const val K_AUTH = "auth"
        const val K_HA1 = "ha1"
        const val K_PW = "pw"
        const val K_AUTH_SCHEME = "auth_scheme"        // digest | aka
        const val K_AKA_K = "aka_k"                    // ⚠️ 평문 저장(개발용) — 운영은 Keystore 교체 대상(비번과 동일)
        const val K_AKA_OPC = "aka_opc"
        const val K_AKA_AMF = "aka_amf"
        const val K_SEC_MECH = "sec_mech"              // 서버 제시 sec-agree 목록 CSV
        const val K_EXPIRES = "expires"
        const val K_CC = "cc"
        const val K_SDS_CPLANE_MAX = "sds_cplane_max"
        const val K_MANUAL = "manual"
    }
}
