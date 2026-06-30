package com.cims.ue.core.provision

import okhttp3.FormBody
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/**
 * 로그인 + 자동 프로비저닝 클라이언트 (설계서 android_ue_provisioning.md).
 *
 *  1) [login] : IdMS OAuth2 PKCE(S256) — `/idms/authreq` → `/idms/tokenreq` (TS 33.180)
 *  2) [fetchProfile] : `GET /provisioning/me` (Bearer) → 서비스별 [ProvisioningProfile]
 *
 * VoLTE·PTT 공용(core). 각 앱은 [ProvisioningProfile.service] 로 자기 kind 프로파일을 골라
 * [ServiceProfile.toSipAccountConfig] 로 단말 설정을 자동 구성한다.
 *
 * ⚠️ 호출은 블로킹(OkHttp execute) — IO 스레드/코루틴에서 호출. 서버 엔드포인트 미준비 시 예외 → 호출자는 수동설정 fallback.
 */
class ProvisioningClient(
    private val csc: CscEndpoint,
    allowInsecureTls: Boolean = false,
) {
    private val http: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .apply { if (allowInsecureTls) insecure(this) }
        .build()

    /** IdMS PKCE 로그인 → 토큰. */
    fun login(userName: String, password: String): TokenSet {
        val verifier = Pkce.newVerifier()
        val code = requestAuthCode(userName, password, Pkce.challenge(verifier), Pkce.newState())
        return requestToken(code, verifier)
    }

    private fun requestAuthCode(userName: String, password: String, challenge: String, state: String): String {
        val url = "${csc.baseUrl}/idms/authreq".toHttpUrl().newBuilder()
            .addQueryParameter("user_name", userName)
            .addQueryParameter("user_password", password)
            .addQueryParameter("client_id", csc.clientId)
            .addQueryParameter("redirect_uri", csc.redirectUri)
            .addQueryParameter("code_challenge", challenge)
            .addQueryParameter("code_challenge_method", "S256")
            .addQueryParameter("scope", csc.scope)
            .addQueryParameter("state", state)
            .build()
        http.newCall(Request.Builder().url(url).get().build()).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "authreq ${resp.code}: $body" }
            return JSONObject(body).getString("code")
        }
    }

    private fun requestToken(code: String, verifier: String): TokenSet {
        val form = FormBody.Builder()
            .add("grant_type", "authorization_code")
            .add("code", code)
            .add("client_id", csc.clientId)
            .add("redirect_uri", csc.redirectUri)
            .add("code_verifier", verifier)
            .build()
        http.newCall(Request.Builder().url("${csc.baseUrl}/idms/tokenreq").post(form).build()).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "tokenreq ${resp.code}: $body" }
            val j = JSONObject(body)
            return TokenSet(
                accessToken = j.getString("access_token"),
                tokenType = j.optString("token_type", "Bearer"),
                refreshToken = j.optString("refresh_token", null),
                idToken = j.optString("id_token", null),
                expiresInSec = j.optInt("expires_in", 3600),
                scope = j.optString("scope", null),
            )
        }
    }

    /**
     * refresh_token grant → 새 access_token (+회전된 refresh_token). [scope] 지정 시 원 grant 의
     * subset 으로 좁혀 발급(AccountManager 가 provisioning / mcptt 용도별 토큰을 따로 받기 위함).
     */
    fun refresh(refreshToken: String, scope: String? = null): TokenSet {
        val fb = FormBody.Builder()
            .add("grant_type", "refresh_token")
            .add("refresh_token", refreshToken)
            .add("client_id", csc.clientId)
        if (!scope.isNullOrBlank()) fb.add("scope", scope)
        http.newCall(Request.Builder().url("${csc.baseUrl}/idms/tokenreq").post(fb.build()).build()).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "refresh ${resp.code}: $body" }
            val j = JSONObject(body)
            return TokenSet(
                accessToken = j.getString("access_token"),
                tokenType = j.optString("token_type", "Bearer"),
                refreshToken = j.optString("refresh_token", null),
                idToken = j.optString("id_token", null),
                expiresInSec = j.optInt("expires_in", 3600),
                scope = j.optString("scope", null),
            )
        }
    }

    /** GET /provisioning/directory → 회사 전화번호부(조직 트리 + VoLTE 가입자, 읽기전용). */
    fun fetchDirectory(accessToken: String): com.cims.ue.core.contacts.CompanyDirectory {
        val req = Request.Builder()
            .url("${csc.baseUrl}/provisioning/directory")
            .addHeader("Authorization", "Bearer $accessToken")
            .get().build()
        http.newCall(req).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "directory ${resp.code}: $body" }
            val j = JSONObject(body)
            val orgArr = j.optJSONArray("orgs") ?: JSONArray()
            val memArr = j.optJSONArray("entries") ?: JSONArray()
            val orgs = (0 until orgArr.length()).map { i ->
                val o = orgArr.getJSONObject(i)
                com.cims.ue.core.contacts.CompanyOrg(
                    code = o.optString("code"), name = o.optString("name"),
                    parent = o.optString("parent"), sort = o.optInt("sort"))
            }
            val members = (0 until memArr.length()).map { i ->
                val o = memArr.getJSONObject(i)
                com.cims.ue.core.contacts.CompanyContact(
                    orgCode = o.optString("org"), name = o.optString("name"), number = o.optString("msisdn"))
            }
            return com.cims.ue.core.contacts.CompanyDirectory(orgs, members)
        }
    }

    /** GET /provisioning/me → 서비스별 프로파일. */
    fun fetchProfile(accessToken: String): ProvisioningProfile {
        val req = Request.Builder()
            .url("${csc.baseUrl}/provisioning/me")
            .addHeader("Authorization", "Bearer $accessToken")
            .get().build()
        http.newCall(req).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "provisioning ${resp.code}: $body" }
            return parse(JSONObject(body))
        }
    }

    private fun parse(j: JSONObject): ProvisioningProfile {
        val user = j.optJSONObject("user")
        val arr = j.optJSONArray("services") ?: JSONArray()
        val services = (0 until arr.length()).map { i ->
            val s = arr.getJSONObject(i)
            val sip = s.getJSONObject("sip")
            val acc = s.getJSONObject("account")
            ServiceProfile(
                kind = s.getString("kind"),
                sipHost = sip.getString("host"),
                sipPort = sip.optInt("port", 5060),
                transport = sip.optString("transport", "UDP"),
                domain = sip.getString("domain"),
                msisdn = acc.optString("msisdn", ""),
                imsi = acc.optString("imsi", ""),
                authId = acc.optString("authId", ""),
                sipPassword = acc.optString("sipPassword", null),
                mcpttId = acc.optString("mcpttId", null),
            )
        }
        return ProvisioningProfile(
            displayName = user?.optString("displayName", null),
            loginId = user?.optString("loginId", null),
            services = services,
        )
    }

    private fun insecure(b: OkHttpClient.Builder) {
        val tm = object : javax.net.ssl.X509TrustManager {
            override fun checkClientTrusted(c: Array<out java.security.cert.X509Certificate>?, a: String?) {}
            override fun checkServerTrusted(c: Array<out java.security.cert.X509Certificate>?, a: String?) {}
            override fun getAcceptedIssuers() = arrayOf<java.security.cert.X509Certificate>()
        }
        val ctx = javax.net.ssl.SSLContext.getInstance("TLS").apply { init(null, arrayOf(tm), java.security.SecureRandom()) }
        b.sslSocketFactory(ctx.socketFactory, tm)
        b.hostnameVerifier { _, _ -> true }
    }
}
