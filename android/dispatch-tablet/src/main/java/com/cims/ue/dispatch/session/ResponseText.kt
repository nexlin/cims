// 응답 코드·오류 토큰 → 화면 문구 (dispatch_desktop_ui.md §9 사전이 정본)
//
// 문구를 새로 짓지 않는다 — 데스크톱 앱(`Services/ResponseText.cs`)과 **같은 문장**을 쓴다.
// 보완이 필요하면 정본 문서를 먼저 고친다(android_dispatch_tablet.md §10).
package com.cims.ue.dispatch.session

import org.json.JSONObject

/** 문구 영역 — 같은 상태코드라도 화면에 따라 다르게 읽힌다. */
enum class TextArea { MANAGEMENT, RECORDING, GROUP }

object ResponseText {

    /** 상태코드 → 문구. 영역별 사전에 없으면 null. */
    fun forStatus(area: TextArea, code: Int): String? = when (area to code) {
        TextArea.MANAGEMENT to 401, TextArea.RECORDING to 401, TextArea.GROUP to 401 ->
            "로그인이 만료됐습니다 — 다시 로그인하세요"
        TextArea.MANAGEMENT to 403 -> "관리 권한이 없습니다 (관제 그룹 관리 범위)"
        TextArea.MANAGEMENT to 404 -> "대상이 없습니다 — 목록을 새로 고칩니다"
        TextArea.MANAGEMENT to 409 -> "충돌 — 이미 있거나 비어 있지 않습니다"
        TextArea.RECORDING to 403 -> "청취 범위 밖의 녹취입니다"
        TextArea.RECORDING to 404 -> "녹취가 없습니다"
        TextArea.RECORDING to 500 -> "녹취 변환에 실패했습니다 — [다시 변환]"
        TextArea.RECORDING to 502 -> "녹취 서버(OAM)에 닿지 않습니다"
        TextArea.GROUP to 403 -> "그룹을 만들거나 바꿀 권한이 없습니다 (자격 또는 본인 소유 그룹만)"
        TextArea.GROUP to 404 -> "그룹이 없습니다 — 목록을 새로 고칩니다"
        TextArea.GROUP to 409 -> "같은 id 의 그룹을 다른 사용자가 소유하고 있습니다"
        TextArea.GROUP to 412 -> "다른 곳에서 먼저 바뀐 그룹입니다 — 다시 열어 편집하세요"
        TextArea.GROUP to 400 -> "그룹 문서 형식 오류"
        else -> null
    }

    /**
     * 관리 API 오류 본문의 `error` → 문구.
     *
     * `where` 는 `number_exists` 의 충돌 위치(가입 테이블 이름 또는 `phone_groups` = 대표번호 주소 공간).
     * 서버 `admin.py` 가입 경로는 토큰이 아니라 **문장**을 내므로 접두 매칭으로 받는다.
     */
    fun forManagementError(error: String, detail: String = "", where: String = ""): String? = when {
        error == "no_directory_admin" ->
            "관리 권한이 없습니다 — 관제 그룹의 관리 범위(directory_admin)를 콘솔에서 부여해야 합니다"
        error == "out_of_scope" -> "관리 범위 밖의 조직·구성원입니다"
        error == "insufficient_scope" -> "토큰 권한이 부족합니다 — 다시 로그인하세요"
        error == "not_editable" ->
            "원격 청취 자격은 관제 앱에서 바꿀 수 없습니다 — 콘솔에서 역할(청취 범위)로 부여합니다"
        error == "schema_not_migrated" ->
            if (detail.contains("voip_subscriptions"))
                "유선 VoIP 회선 테이블이 아직 없습니다 — 서버 DB 마이그레이션(migrate_voip_subscriptions.sql)이 필요합니다(운영자)"
            else "서버 DB 마이그레이션이 필요합니다 (관리자 문의)"
        error == "code_exists" -> "같은 코드의 조직이 이미 있습니다"
        error == "unknown_parent" -> "상위 조직이 없습니다"
        error == "unknown_org" -> "소속 조직이 없습니다"
        error == "cyclic_parent" -> "자기 하위 조직으로 옮길 수 없습니다"
        error == "not_empty" ->
            if (detail.contains("members")) "구성원이 남아 있는 조직은 지울 수 없습니다"
            else "하위 조직이 남아 있는 조직은 지울 수 없습니다"
        error == "number_exists" -> when (where) {
            "phone_groups" -> "전화 그룹의 대표번호로 쓰이는 번호입니다 — 가입 번호로 개설할 수 없습니다"
            "volte_subscriptions" -> "다른 VoLTE 회선이 쓰는 번호입니다"
            "voip_subscriptions" -> "다른 VoIP 회선이 쓰는 번호입니다"
            "ptt_subscriptions" -> "다른 PTT 회선이 쓰는 번호입니다"
            else -> "다른 구성원이 쓰는 번호입니다"
        }
        error == "self_delete" -> "자기 자신은 지울 수 없습니다"
        error == "derived_from_phone_group" ->
            "전화 그룹 소속 구성원의 픽업 그룹은 콘솔 전화 그룹에서 파생됩니다 — 직접 바꿀 수 없습니다"
        error == "service_kind_mismatch" ->
            "회선 종류에 맞지 않는 접속서비스입니다 — VoLTE 회선은 volte, VoIP 회선은 voip, PTT 회선은 ptt 서비스만 고를 수 있습니다"
        error == "no_monitor_scope" -> "관제 그룹 미소속 — 이력·녹취를 볼 수 없습니다"
        error == "oam_unreachable" -> "녹취 서버(OAM)에 닿지 않습니다"
        error == "invalid_recording_id" -> "녹취 식별자가 잘못됐습니다"
        error == "service_log_unavailable" -> "서버 녹취 저장소에 닿지 않습니다"
        error == "db_unavailable" || error == "db_error" -> "서버 DB 오류 — 잠시 후 다시 시도"
        error.startsWith("passwd required when imsi or service_ref") ->
            "저장된 IMSI·접속서비스와 달라 H(A1) 재결박이 필요합니다 — SIP 비밀번호를 함께 입력하세요"
        error.startsWith("password required when changing the number") ->
            "번호를 바꾸려면 SIP 비밀번호가 필요합니다(H(A1) 재결박)"
        error.startsWith("service_ref required for voip") ->
            "VoIP(유선) 회선은 유선(voip) 접속서비스를 골라야 합니다 — 후보가 비어 있으면 서버 접속서비스(kind=voip) 등록이 필요합니다(운영자)"
        error.startsWith("service_ref required to derive ha1") ->
            "접속서비스를 알 수 없어 H(A1) 을 만들 수 없습니다 — 접속서비스를 고르세요. 목록이 비어 있으면 서버 csc.json Provisioning.Services 설정이 필요합니다(운영자)"
        error.startsWith("imsi required") -> "IMSI 가 필요합니다(비우면 번호 숫자로 채워집니다)"
        error.startsWith("sip_transport must be") ->
            "SIP transport 는 UDP/TCP/TLS/ANY 중 하나여야 합니다(ANY = 접속서비스 기본)"
        else -> null
    }

    /** GMS 오류 본문의 `error` → 문구(mcptt_api.md §2 표). */
    fun forGroupError(error: String, detail: String = ""): String? = when (error) {
        "group_creation_not_allowed" -> "그룹 생성 자격이 없습니다 (관리자 부여 필요)"
        "not_group_owner" -> "본인이 만든 그룹만 편집·삭제할 수 있습니다"
        "uri_taken" -> "같은 id 의 그룹을 다른 사용자가 소유하고 있습니다 — 다른 id 로 다시 시도"
        "unknown_member" ->
            if (detail.isNotEmpty()) "PTT 미가입 번호가 있습니다: $detail" else "PTT 미가입 번호가 있습니다"
        "invalid_group_id", "reserved_prefix", "invalid_group_document" -> "그룹 문서 형식 오류 (앱 결함 — 로그 확인)"
        "etag_mismatch" -> "편집 중 다른 곳에서 먼저 바뀐 그룹입니다 — 문서를 다시 읽어 편집하세요"
        "not_found" -> "그룹이 없습니다 — 목록을 새로 고칩니다"
        else -> null
    }

    /**
     * 실패 한 건을 문장으로. 본문이 JSON 이면 `error`·`detail`·`where` 를 뽑아 세분 사전을 먼저 본다.
     *
     * 사전에 없으면 서버 문장을 그대로 보인다 — 조용히 삼키면 원인을 못 찾는다.
     */
    fun of(area: TextArea, code: Int, body: String): String {
        val (error, detail, where) = parse(body)
        val fine = when (area) {
            TextArea.GROUP -> forGroupError(error, detail)
            else -> forManagementError(error, detail, where)
        }
        if (fine != null) return fine
        forStatus(area, code)?.let { return it }
        if (code < 0) return "서버에 닿지 않습니다 ($code)"
        return if (error.isNotEmpty()) "$error ($code)" else "요청 실패 ($code)"
    }

    /** 오류 본문 → (error, detail, where). JSON 이 아니면 본문 전체를 error 로 본다. */
    internal fun parse(body: String): Triple<String, String, String> {
        val s = body.trim()
        if (!s.startsWith("{")) return Triple(s, "", "")
        return try {
            val o = JSONObject(s)
            val detail = when {
                o.isNull("detail") -> ""
                else -> o.get("detail").toString()
            }
            Triple(o.optString("error", ""), detail, o.optString("where", ""))
        } catch (_: Exception) {
            Triple(s, "", "")
        }
    }
}
