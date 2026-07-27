package com.cims.ue.core.account

import android.accounts.Account
import android.accounts.AccountManager
import android.content.Context
import android.os.Bundle
import com.cims.ue.core.provision.CscEndpoint

/**
 * CIMS 단일 로그인(SSO) 공유 계정 — AccountManager 기반. 모든 앱(CIMS/CIMS-Phone/CIMS-McPtt)이
 * 같은 계정타입을 참조하고, **authTokenType 으로 용도(평면)를 분리**한다:
 *  - [TOKEN_PROVISIONING] : CIMS 앱 부트스트랩(/provisioning/me) — scope `cims:provisioning`
 *  - [TOKEN_MCPTT]        : MCPTT 서비스 평면(XCAP/KMS/affiliation, TS 33.180) — scope `3gpp:mcptt:ptt_server`
 *
 * 계정(로그인)은 1개, refresh_token 은 owner(CIMS) 앱이 보관(setPassword). 다른 앱은
 * [AccountManager.getAuthToken] 으로 용도별 토큰을 받는다(동일 서명키 → signature 수준 접근).
 * VoLTE 는 토큰 없이 프로비저닝으로 받은 SIP Digest 계정만 사용.
 */
object CimsAccounts {
    const val ACCOUNT_TYPE = "com.cims.ue"

    const val TOKEN_PROVISIONING = "cims.provisioning"
    const val TOKEN_MCPTT = "3gpp:mcptt:ptt_server"

    /** owner(CIMS) 앱의 로그인 Activity 를 띄우는 액션. */
    const val ACTION_LOGIN = "com.cims.ue.account.LOGIN"

    // 계정 userData
    const val KEY_CSC_HOST = "csc_host"
    const val KEY_CSC_PORT = "csc_port"
    const val KEY_PROFILE_JSON = "profile_json"   // /provisioning/me 캐시(선택)
    // 로그인 비번 — VoLTE/PTT 의 SIP Digest 비번 재사용용(서버가 sipPassword=null 로 내릴 때).
    //   IMS Digest 는 토큰 인증이 없으므로 비번이 필요. AccountManager(동일서명 프로세스 보호)에 보관.
    //   ⚠️ 현재 평문(개발) — ConfigStore 와 동일 posture. 운영 시 Keystore/EncryptedSharedPreferences.
    const val KEY_LOGIN_PW = "login_pw"

    fun loginPassword(am: AccountManager, account: Account): String =
        am.getUserData(account, KEY_LOGIN_PW).orEmpty()

    /** authTokenType → IdMS scope (서버 SCOPE_PROVISIONING / SCOPE_MCPTT 와 정합). */
    fun scopeFor(tokenType: String): String = when (tokenType) {
        TOKEN_MCPTT -> "3gpp:mcptt:ptt_server"
        else -> "cims:provisioning"
    }

    fun get(am: AccountManager): Account? = am.getAccountsByType(ACCOUNT_TYPE).firstOrNull()
    fun get(context: Context): Account? = get(AccountManager.get(context))

    fun cscEndpoint(am: AccountManager, account: Account): CscEndpoint {
        val host = am.getUserData(account, KEY_CSC_HOST).orEmpty()
        val port = am.getUserData(account, KEY_CSC_PORT)?.toIntOrNull() ?: 4430
        return CscEndpoint(host = host, port = port)
    }

    /**
     * 블로킹 토큰 획득(**IO 스레드/코루틴에서만** 호출). 캐시 없으면 authenticator 가 refresh 로 발급.
     * 토큰 없음(계정 문제) 시 null.
     *
     * ⚠️ owner(CIMS) 앱 프로세스가 죽어 있으면(강제종료·제조사 백그라운드 킬러 등) 시스템의
     * 인증자 서비스 바인딩이 `AuthenticatorException("bind failure")` 로 일시 실패할 수 있다
     * → 짧게 1회 재시도(시스템이 owner 프로세스를 띄울 시간), 그래도 실패면 원인 메시지로 던진다.
     */
    fun blockingToken(am: AccountManager, account: Account, tokenType: String): String? {
        var last: Exception? = null
        repeat(2) { attempt ->
            try {
                val bundle: Bundle = am.getAuthToken(account, tokenType, null, false, null, null).result
                    ?: return null
                return bundle.getString(AccountManager.KEY_AUTHTOKEN)
            } catch (e: Exception) {
                last = e
                if (attempt == 0) Thread.sleep(700)
            }
        }
        throw IllegalStateException("CIMS 앱(계정 서비스) 연결 실패 — 잠시 후 다시 시도하세요", last)
    }

    /** 만료 의심 토큰 캐시 무효화 → 다음 호출 시 refresh 재발급. */
    fun invalidate(am: AccountManager, token: String?) {
        if (!token.isNullOrEmpty()) am.invalidateAuthToken(ACCOUNT_TYPE, token)
    }

    /**
     * 미로그인(공유 계정 없음, 수동 설정 모드 아님)이면 CIMS 오너 앱 로그인 화면으로 전환한다.
     * companion 앱(Phone/PTT)이 Activity 진입 시 호출 — true 반환 시 호출자는 자기 화면을
     * finish 한다. CIMS 앱 미설치 등으로 전환 실패면 false(호출자는 자체 안내 화면 폴백).
     */
    fun redirectToLoginIfLoggedOut(activity: android.app.Activity): Boolean {
        if (get(activity) != null) return false
        if (com.cims.ue.core.config.ConfigStore(activity).isManual()) return false
        return runCatching {
            activity.startActivity(
                android.content.Intent(ACTION_LOGIN)
                    .setPackage(com.cims.ue.core.CimsSuite.CIMS_PACKAGE)
                    .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK),
            )
            true
        }.getOrDefault(false)
    }
}
