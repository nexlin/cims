package com.cims.ue.core.account

import android.accounts.AccountManager
import android.content.Context
import com.cims.ue.core.provision.ProvisioningClient
import com.cims.ue.core.provision.ProvisioningProfile

/**
 * 공유 계정(SSO)으로 자동 프로비저닝 — 앱이 로그인 UI 없이 자기 서비스 설정을 받는다.
 * **provisioning 토큰**(scope=cims:provisioning)으로 `/provisioning/me` 를 조회한다.
 * (MCPTT 서비스 평면 토큰은 [CimsAccounts.TOKEN_MCPTT] 로 별도 획득 — 평면 분리.)
 *
 * ⚠️ 블로킹(IO 스레드/서비스 워커에서 호출). 계정 없거나 실패 시 null → 호출자는 수동설정 fallback.
 */
object SsoProvisioner {

    fun fetchProfile(context: Context): ProvisioningProfile? {
        val am = AccountManager.get(context)
        val account = CimsAccounts.get(am) ?: return null
        val token = CimsAccounts.blockingToken(am, account, CimsAccounts.TOKEN_PROVISIONING) ?: return null
        val ep = CimsAccounts.cscEndpoint(am, account)
        return try {
            ProvisioningClient(ep, allowInsecureTls = true).fetchProfile(token)
        } catch (e: Exception) {
            // 캐시된 토큰이 만료됐을 수 있음 → 무효화 후 1회 재시도
            CimsAccounts.invalidate(am, token)
            val retry = CimsAccounts.blockingToken(am, account, CimsAccounts.TOKEN_PROVISIONING) ?: return null
            runCatching { ProvisioningClient(ep, allowInsecureTls = true).fetchProfile(retry) }.getOrNull()
        }
    }

    /**
     * 회사 전화번호부 동기화(`/provisioning/directory`). [knownEtag] 를 보내 서버 버전이 같으면
     * 변경 없음(changed=false)로 응답(다운로드 생략). 만료 토큰 1회 재시도.
     * **어떤 실패든 null** (UI 는 "동기화 실패" 안내) — 토큰 획득 실패(bind failure 등)도 포함.
     */
    fun fetchDirectory(context: Context, knownEtag: String? = null): com.cims.ue.core.contacts.DirectorySync? = runCatching {
        val am = AccountManager.get(context)
        val account = CimsAccounts.get(am) ?: return null
        val token = CimsAccounts.blockingToken(am, account, CimsAccounts.TOKEN_PROVISIONING) ?: return null
        val ep = CimsAccounts.cscEndpoint(am, account)
        try {
            ProvisioningClient(ep, allowInsecureTls = true).fetchDirectory(token, knownEtag)
        } catch (e: Exception) {
            CimsAccounts.invalidate(am, token)
            val retry = CimsAccounts.blockingToken(am, account, CimsAccounts.TOKEN_PROVISIONING) ?: return null
            runCatching { ProvisioningClient(ep, allowInsecureTls = true).fetchDirectory(retry, knownEtag) }.getOrNull()
        }
    }.getOrNull()

    /** 로그인(공유 계정) 존재 여부. */
    fun hasAccount(context: Context): Boolean = CimsAccounts.get(context) != null

    /** 공유 계정의 로그인 비번(SIP Digest 재사용용). 없으면 빈 문자열. */
    fun loginPassword(context: Context): String {
        val am = AccountManager.get(context)
        val account = CimsAccounts.get(am) ?: return ""
        return CimsAccounts.loginPassword(am, account)
    }
}
