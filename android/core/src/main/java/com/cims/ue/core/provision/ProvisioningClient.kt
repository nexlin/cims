package com.cims.ue.core.provision

import com.cims.ue.core.config.SipAccountConfig
import com.cims.ue.core.net.CimsTls
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
) {
    private val http: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        // CIMS 사설 CA 로 서버 인증서를 검증한다(CimsTls). 이 채널로 로그인 비번·SIP 접속 정보가
        //   오가므로 검증을 끄면 중간자에게 그대로 넘어간다.
        .apply { CimsTls.apply(this) }
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

    /**
     * GET /provisioning/directory → 회사 전화번호부(조직 트리 + VoLTE 가입자, 읽기전용).
     * [knownEtag] 를 If-None-Match 로 보내 서버 버전이 같으면 304 → `changed=false`(다운로드 생략).
     */
    fun fetchDirectory(
        accessToken: String,
        knownEtag: String? = null,
        service: String = "volte",
    ): com.cims.ue.core.contacts.DirectorySync {
        val b = Request.Builder()
            .url("${csc.baseUrl}/provisioning/directory?service=$service")
            .addHeader("Authorization", "Bearer $accessToken")
        if (!knownEtag.isNullOrBlank()) b.addHeader("If-None-Match", knownEtag)
        http.newCall(b.get().build()).execute().use { resp ->
            if (resp.code == 304) {
                return com.cims.ue.core.contacts.DirectorySync(changed = false, dir = null, etag = knownEtag)
            }
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "directory ${resp.code}: $body" }
            val newEtag = resp.header("ETag") ?: knownEtag
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
            return com.cims.ue.core.contacts.DirectorySync(
                changed = true,
                dir = com.cims.ue.core.contacts.CompanyDirectory(orgs, members),
                etag = newEtag)
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
            // 가용 transport 목록 — 서버가 알린 선택지. 목록이 없거나 항목이 온전치 않으면
            //   그 항목만 버린다(빈 목록 = 구 서버 → 단말은 단일 필드로만 동작).
            val tpArr = sip.optJSONArray("transports") ?: JSONArray()
            val transports = (0 until tpArr.length()).mapNotNull { k ->
                val t = tpArr.getJSONObject(k)
                val tp = runCatching {
                    SipAccountConfig.Transport.valueOf(t.optString("transport").uppercase())
                }.getOrNull()
                val port = t.optInt("port", 0)
                if (tp != null && port in 1..65535) SipAccountConfig.TransportEndpoint(tp, port) else null
            }
            // 기본값 = sip.default(신규) → sip.transport(구 서버 호환).
            val defaultTransport = sip.optString("default", "").ifBlank { sip.optString("transport", "UDP") }
            ServiceProfile(
                kind = s.getString("kind"),
                sipHost = sip.getString("host"),
                sipPort = sip.optInt("port", 5060),
                transport = defaultTransport,
                transports = transports,
                domain = sip.getString("domain"),
                msisdn = acc.optString("msisdn", ""),
                imsi = acc.optString("imsi", ""),
                authId = acc.optString("authId", ""),
                sipHa1 = acc.stringOrNull("sipHa1"),
                sipPassword = acc.stringOrNull("sipPassword"),
                authScheme = acc.optString("authScheme", "digest").ifBlank { "digest" },
                akaK = acc.optJSONObject("aka")?.optString("k").orEmpty(),
                akaOpc = acc.optJSONObject("aka")?.optString("opc").orEmpty(),
                akaAmf = acc.optJSONObject("aka")?.optString("amf").orEmpty().ifBlank { "8000" },
                secMechanisms = (sip.optJSONArray("security") ?: JSONArray()).let { sec ->
                    (0 until sec.length()).map { k -> sec.optString(k) }.filter { it.isNotBlank() }
                },
                mediaSecurity = sip.optString("mediaSecurity", "off").ifBlank { "off" },
                mcpttId = acc.stringOrNull("mcpttId"),
                maxPayloadSdsCplaneBytes = s.optJSONObject("mcdata")
                    ?.optInt("maxPayloadSdsCplaneBytes", 0) ?: 0,
            )
        }
        return ProvisioningProfile(
            displayName = user?.stringOrNull("displayName"),
            loginId = user?.stringOrNull("loginId"),
            countryCode = j.stringOrNull("countryCode")?.takeIf { it.isNotBlank() },
            services = services,
        )
    }

    /** JSON 명시적 null 안전 문자열 추출 — org.json optString 은 명시적 null 을 "null" 문자열로 만든다. */
    private fun JSONObject.stringOrNull(name: String): String? =
        if (isNull(name)) null else optString(name, null)
}
