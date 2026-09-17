package com.cims.ue.core.account

import android.accounts.AbstractAccountAuthenticator
import android.accounts.Account
import android.accounts.AccountAuthenticatorResponse
import android.accounts.AccountManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.util.Log
import com.cims.ue.core.provision.ProvisioningClient

/**
 * CIMS 공유 계정 인증자 — owner(CIMS) 앱의 [AuthenticatorService] 가 호스팅.
 *
 * [getAuthToken] 은 계정에 보관된 refresh_token 으로 **authTokenType 별 scope** 토큰을 발급한다
 * (provisioning ↔ mcptt 평면 분리). 캐시가 있고 만료 전이면 그대로, 없거나 만료(임박)면 refresh, 실패하면
 * 재로그인 Intent 반환. AccountManager 는 토큰 만료를 모르므로 만료 판정은 여기서 한다([JwtClaims]).
 */
class CimsAuthenticator(private val context: Context) : AbstractAccountAuthenticator(context) {

    private companion object { const val TAG = "CimsAuth" }

    override fun getAuthToken(
        response: AccountAuthenticatorResponse?, account: Account,
        authTokenType: String, options: Bundle?,
    ): Bundle {
        val am = AccountManager.get(context)
        var token = am.peekAuthToken(account, authTokenType)
        if (!token.isNullOrEmpty() && JwtClaims.isExpiring(token)) {
            // 만료(임박) 캐시 — 그대로 내주면 1시간 뒤 모든 GMS/CMS 호출이 401 로 끝난다. 버리고 refresh 로.
            am.invalidateAuthToken(account.type, token)
            token = null
        }
        if (token.isNullOrEmpty()) {
            val refresh = am.getPassword(account)
            if (refresh.isNullOrEmpty()) {
                // refresh_token 이 없다 = 로그인한 적 없거나 로그아웃/초기화됨. 재로그인 외 방법이 없다.
                Log.w(TAG, "$authTokenType: refresh_token 없음 — 재로그인 필요")
                return reLogin(response, account.name, authTokenType)
            }
            try {
                val ep = CimsAccounts.cscEndpoint(am, account)
                val ts = ProvisioningClient(ep)
                    .refresh(refresh, CimsAccounts.scopeFor(authTokenType))
                token = ts.accessToken
                if (!ts.refreshToken.isNullOrEmpty()) am.setPassword(account, ts.refreshToken) // 회전 반영
                am.setAuthToken(account, authTokenType, token)
                Log.i(TAG, "$authTokenType: 토큰 갱신 (${ep.host}:${ep.port})")
            } catch (e: Exception) {
                // 조용히 실패하면 호출자는 만료 토큰으로 401 을 계속 맞는다 — 원인을 남긴다.
                Log.w(TAG, "$authTokenType: 토큰 갱신 실패 — ${e.javaClass.simpleName}: ${e.message}")
                return reLogin(response, account.name, authTokenType)
            }
        }
        if (!token.isNullOrEmpty()) {
            return Bundle().apply {
                putString(AccountManager.KEY_ACCOUNT_NAME, account.name)
                putString(AccountManager.KEY_ACCOUNT_TYPE, account.type)
                putString(AccountManager.KEY_AUTHTOKEN, token)
            }
        }
        return reLogin(response, account.name, authTokenType)
    }

    override fun addAccount(
        response: AccountAuthenticatorResponse?, accountType: String?,
        authTokenType: String?, requiredFeatures: Array<out String>?, options: Bundle?,
    ): Bundle = reLogin(response, null, authTokenType)

    /** owner(CIMS) 앱의 로그인 Activity 로 보내는 Intent 번들. */
    private fun reLogin(response: AccountAuthenticatorResponse?, accountName: String?, tokenType: String?): Bundle {
        val intent = Intent(CimsAccounts.ACTION_LOGIN).apply {
            setPackage(context.packageName)   // authenticator 는 owner(CIMS) 프로세스 → 자기 앱의 LoginActivity
            putExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE, response)
            accountName?.let { putExtra(AccountManager.KEY_ACCOUNT_NAME, it) }
            putExtra("authTokenType", tokenType)
        }
        return Bundle().apply { putParcelable(AccountManager.KEY_INTENT, intent) }
    }

    override fun getAuthTokenLabel(authTokenType: String): String = when (authTokenType) {
        CimsAccounts.TOKEN_MCPTT -> "MC 서비스 (TS 33.180)"
        else -> "CIMS 프로비저닝"
    }

    override fun editProperties(response: AccountAuthenticatorResponse?, accountType: String?): Bundle = Bundle()
    override fun confirmCredentials(response: AccountAuthenticatorResponse?, account: Account?, options: Bundle?): Bundle? = null
    override fun updateCredentials(response: AccountAuthenticatorResponse?, account: Account?, authTokenType: String?, options: Bundle?): Bundle = Bundle()
    override fun hasFeatures(response: AccountAuthenticatorResponse?, account: Account?, features: Array<out String>?): Bundle =
        Bundle().apply { putBoolean(AccountManager.KEY_BOOLEAN_RESULT, false) }
}
