package com.cims.ue.ptt.csc

import com.cims.ue.core.provision.Pkce
import okhttp3.FormBody
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.net.URLEncoder
import java.util.concurrent.TimeUnit

/**
 * CSC 설정 플레인 클라이언트 — IdMS(OAuth2 PKCE) + GMS/CMS(XCAP) HTTPS, **PJSIP 무관** (설계서 §7).
 *
 * 규격: 3GPP TS 33.180(IdMS/OIDC) · TS 24.481(GMS) · TS 24.484(CMS). 경로/Content-Type 은
 * 서버(csc) 와 정합:
 *  - IdMS:  GET /idms/authreq , POST /idms/tokenreq
 *  - GMS:   GET /org.openmobilealliance.groups/users/{me}[/{group}]  (목록=JSON, 문서=vnd.oma.poc.groups+xml)
 *  - CMS:   GET /org.3gpp.mcptt.user-profile|service-config/users/{me}/...
 *
 * 부팅 순서: IdMS 인증 → GMS/CMS 조회 → (이후 SIP REGISTER/affiliate/INVITE). ETag/If-None-Match 캐시.
 *
 * ⚠️ 호출은 블로킹(OkHttp execute). IO 스레드/코루틴(Dispatchers.IO)에서 호출할 것.
 */
class CscClient(
    private val cfg: CscConfig,
    /** 개발 서버 자체서명 인증서 허용(운영 금지). */
) {
    private val http: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .apply { com.cims.ue.core.net.CimsTls.apply(this) }   // CIMS 사설 CA 로 서버 인증서 검증
        .build()

    // ── IdMS (OAuth2 PKCE, S256) ──

    /**
     * 전체 PKCE 인증: authreq(code) → tokenreq(token). verifier 는 내부 생성·검증에 사용.
     * @return 발급된 [TokenSet]
     */
    fun authenticate(userName: String, userPassword: String): TokenSet {
        val verifier = Pkce.newVerifier()
        val code = requestAuthCode(userName, userPassword, Pkce.challenge(verifier), Pkce.newState())
        return requestToken(code, verifier)
    }

    /** GET /idms/authreq → 인증 코드. */
    fun requestAuthCode(userName: String, userPassword: String, codeChallenge: String, state: String): String {
        val url = "${cfg.baseUrl}/idms/authreq".toHttpUrl().newBuilder()
            .addQueryParameter("user_name", userName)
            .addQueryParameter("user_password", userPassword)
            .addQueryParameter("client_id", cfg.clientId)
            .addQueryParameter("redirect_uri", cfg.redirectUri)
            .addQueryParameter("code_challenge", codeChallenge)
            .addQueryParameter("code_challenge_method", "S256")
            .addQueryParameter("scope", cfg.scope)
            .addQueryParameter("state", state)
            .build()
        http.newCall(Request.Builder().url(url).get().build()).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "authreq ${resp.code}: $body" }
            return JSONObject(body).getString("code")
        }
    }

    /** POST /idms/tokenreq (authorization_code grant + code_verifier) → 토큰. */
    fun requestToken(code: String, codeVerifier: String): TokenSet {
        val form = FormBody.Builder()
            .add("grant_type", "authorization_code")
            .add("code", code)
            .add("client_id", cfg.clientId)
            .add("redirect_uri", cfg.redirectUri)
            .add("code_verifier", codeVerifier)
            .build()
        http.newCall(Request.Builder().url("${cfg.baseUrl}/idms/tokenreq").post(form).build()).execute().use { resp ->
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

    // ── GMS ──

    /** GET /org.openmobilealliance.groups/users/{me} → 그룹 목록(JSON). */
    fun listGroups(token: String, userUri: String): List<GroupSummary> {
        val url = "${cfg.baseUrl}/org.openmobilealliance.groups/users/${enc(userUri)}"
        http.newCall(bearer(token, url).get().build()).execute().use { resp ->
            val body = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "listGroups ${resp.code}: $body" }
            val arr = JSONArray(body)
            return (0 until arr.length()).map { i ->
                val o = arr.getJSONObject(i)
                GroupSummary(
                    uri = o.getString("uri"),
                    displayName = o.optString("display_name", null),
                    etag = o.optString("etag", null),
                    memberCount = if (o.has("member_count")) o.getInt("member_count") else null,
                )
            }
        }
    }

    /** GET /org.openmobilealliance.groups/users/{me}/{group} → OMA POC 그룹 문서(XML), ETag 캐시. */
    fun getGroupDoc(token: String, userUri: String, groupUri: String, ifNoneMatch: String? = null): XcapDoc =
        xcapGet(token, "/org.openmobilealliance.groups/users/${enc(userUri)}/${enc(groupUri)}",
            accept = "application/vnd.oma.poc.groups+xml", ifNoneMatch = ifNoneMatch)

    // ── CMS ──

    fun getUserProfile(token: String, userUri: String, ifNoneMatch: String? = null): XcapDoc =
        xcapGet(token, "/org.3gpp.mcptt.user-profile/users/${enc(userUri)}/user-profile",
            accept = "application/vnd.3gpp.mcptt-user-profile+xml", ifNoneMatch = ifNoneMatch)

    fun getServiceConfig(token: String, userUri: String, ifNoneMatch: String? = null): XcapDoc =
        xcapGet(token, "/org.3gpp.mcptt.service-config/users/${enc(userUri)}/service-config",
            accept = "application/vnd.3gpp.mcptt-service-config+xml", ifNoneMatch = ifNoneMatch)

    private fun xcapGet(token: String, path: String, accept: String, ifNoneMatch: String?): XcapDoc {
        val req = bearer(token, cfg.baseUrl + path).addHeader("Accept", accept)
        if (ifNoneMatch != null) req.addHeader("If-None-Match", ifNoneMatch)
        http.newCall(req.get().build()).execute().use { resp ->
            if (resp.code == 304) return XcapDoc(notModified = true, body = null, etag = ifNoneMatch, contentType = accept)
            val body = resp.body?.string()
            check(resp.isSuccessful) { "xcap GET $path ${resp.code}: ${body.orEmpty()}" }
            return XcapDoc(
                notModified = false,
                body = body,
                etag = resp.header("ETag") ?: resp.header("Etag"),
                contentType = resp.header("Content-Type") ?: accept,
            )
        }
    }

    // ── MCData FD (파일 업로드/다운로드 — docs/design/features/mcdata_messaging.md) ──

    /** POST /mcdata/fd — 파일 업로드 (그룹 allow_fd·멤버십·크기는 서버가 게이트). */
    fun uploadFd(token: String, data: ByteArray, fileName: String, mime: String, groupId: String): FdUpload {
        val url = "${cfg.baseUrl}/mcdata/fd".toHttpUrl().newBuilder()
            .addQueryParameter("name", fileName)
            .addQueryParameter("group", groupId)
            .addQueryParameter("type", mime)
            .build()
        val body = data.toRequestBody("application/octet-stream".toMediaType())
        http.newCall(bearer(token, url.toString()).post(body).build()).execute().use { resp ->
            val text = resp.body?.string().orEmpty()
            check(resp.isSuccessful) { "fd upload ${resp.code}: $text" }
            val j = JSONObject(text)
            return FdUpload(
                url = j.getString("url"),
                size = j.optLong("size", data.size.toLong()),
                name = j.optString("name", fileName),
            )
        }
    }

    /** FD 파일 다운로드 — FD SIGNALLING 으로 받은 절대 URL 그대로 GET. */
    fun downloadFd(token: String, url: String): ByteArray {
        http.newCall(bearer(token, url).get().build()).execute().use { resp ->
            check(resp.isSuccessful) { "fd download ${resp.code}" }
            return resp.body?.bytes() ?: ByteArray(0)
        }
    }

    private fun bearer(token: String, url: String) =
        Request.Builder().url(url).addHeader("Authorization", "Bearer $token")

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8")
}
