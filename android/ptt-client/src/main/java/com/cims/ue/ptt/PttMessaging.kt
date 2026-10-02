package com.cims.ue.ptt

import android.util.Log
import com.cims.ue.ptt.PttController.Companion.TAG
import com.cims.ue.sdk.FdFile
import com.cims.ue.sdk.RequestResult
import kotlinx.coroutines.launch

/**
 * 메시징 평면 — MCData SDS·FD 발신과 결과 상관(TS 24.282). 본문 조립·media plane(MSRP) 선택·수신 해석은 코어가 한다:
 * 그룹 SDS 가 `AccountConfig.maxSdsCplaneBytes` 를 넘으면 코어가 MSRP 로 보내고 최종 결과가 method "MSRP" 로 온다
 * (ue_sdk.md §4.2 «media plane SDS»). 수신은 [PttController.incomingSds] 하나로 온다.
 */
internal class MessagingPlane(private val c: PttController) {

    /**
     * 문자 발신 — [peer] 가 그룹 ID 면 그룹 SDS(request-type group-sds — CSP MCDATA-AS 가 allow-SDS·멤버십·크기 게이트 후
     * affiliate 멤버에게 fan-out), 아니면 1:1 SDS(one-to-one-sds — 등록 바인딩으로 전달). 로컬 저장은 서비스 몫.
     * 결과는 [PttController.sendResult] — 시그널링 평면은 MESSAGE 최종 응답, media plane 은 저장소 수신.
     * @param msgId 재전송이면 실패한 문자의 msgId(수신측 중복 대사 기준), 아니면 서비스가 새로 만든 값
     * @return message ID — 빈 문자열이면 미발신
     */
    fun sendSds(peer: String, text: String, msgId: String): String {
        if (text.isBlank() || msgId.isBlank()) return ""
        val group = c.isGroupId(peer)
        if (group) {
            // 보내기 전 검사(TS 24.282 §9.2.1.1 1) → §11.1) — 그룹 문서가 문자를 막거나 크기 상한을 넘으면 보내지 않고 알린다
            val doc = c._groupDocs.value[peer]
            CallRules.sdsBlockReason(doc?.allowSds, doc?.maxSdsBytes, text.toByteArray(Charsets.UTF_8).size)?.let { why ->
                c._status.value = "전송 실패 — $why"
                c.feedback?.blocked(why)
                c._sendResult.tryEmit(msgId to false)          // 서비스가 PENDING 으로 저장해 둔 말풍선을 실패로 확정한다
                return ""
            }
        }
        c.ctl.launch {
            val acc = c.account
            val r = when {
                acc == null -> null
                group -> acc.sendGroupSds(peer, text, requestDelivery = true, msgId = msgId)
                else -> acc.sendSds(peer, text, requestDelivery = true, msgId = msgId)
            }
            val sent = r?.getOrNull()
            if (sent == null) {
                Log.w(TAG, "SDS $msgId 발신 실패: ${r?.code} ${r?.reason ?: "not registered"}")
                c._sendResult.tryEmit(msgId to false)
                return@launch
            }
            c.sdsPending[sent.token] = msgId
            c.takeEarly(sent.token)?.let { onSendResult(it) }
        }
        return msgId
    }

    /** SDS(MESSAGE·MSRP) 최종 응답 — 이 평면의 token 이면 true. 401/407 재인증은 엔진이 이미 했다(최종 결과만 온다). */
    fun onSendResult(r: RequestResult): Boolean {
        val msgId = c.sdsPending.remove(r.token) ?: return false
        val ok = r.code in 200..299
        if (!ok) {
            Log.w(TAG, "SDS ${r.method} $msgId 실패: ${r.code} ${r.reason} (Warning ${r.warningCode} ${r.warningText})")
            // 서버가 사유를 준 거절(TS 24.282 §4.9 — 비멤버 116·SDS 꺼짐 206·FD 꺼짐 213·크기 217)은 그 사유를 알린다
            CallRules.sendRejectionText(r.code, r.warningCode)?.let { why -> c._status.value = "전송 실패 — $why"; c.feedback?.blocked(why) }
        } else if (r.method == "MSRP") c._status.value = "대용량 문자 전송 완료"
        c._sendResult.tryEmit(msgId to ok)
        return true
    }

    /**
     * 파일전송 — FD via HTTP (TS 23.282): CSC 콘텐츠 서버 업로드 → FD SIGNALLING PAYLOAD(SIP MESSAGE) 전파.
     * 그룹이면 업로드에 그룹을 실어 서버가 그 그룹의 allow_fd·멤버십으로 게이트한다.
     * @return null 이면 실패 (토큰 없음/업로드 거부 — allow_fd·크기 게이트는 서버가 판정)
     */
    suspend fun sendAttachment(peer: String, data: ByteArray, fileName: String, mime: String): PttController.FdSent? {
        if (c.token == null) { c._status.value = "첨부: 토큰 없음"; return null }
        val group = c.isGroupId(peer)
        val up = c.withToken { cl, t -> cl.uploadFd(t, data, fileName, mime, if (group) peer else "") }
        val u = up.getOrNull() ?: run {
            Log.w(TAG, "FD 업로드 실패: ${up.code} ${up.reason}")
            c._status.value = "첨부 업로드 실패"
            return null
        }
        val file = FdFile(url = u.url, name = u.name.ifBlank { fileName }, type = mime, size = u.size)
        val acc = c.account ?: return null
        val r = if (group) acc.sendGroupFd(peer, file) else acc.sendFd(peer, file)
        val sent = r.getOrNull() ?: run { Log.w(TAG, "FD 알림 실패: ${r.code} ${r.reason}"); return null }
        return PttController.FdSent(sent.msgId, u.url, u.size)
    }

    /** FD 첨부 다운로드 — FILEURL 의 경로만 취해 이 CSC 에서 받는다(Bearer 를 다른 호스트로 보내지 않는다, 코어). */
    suspend fun downloadAttachment(url: String): ByteArray? {
        if (c.token == null) return null
        val r = c.withToken { cl, t -> cl.downloadFd(t, url) }
        return r.getOrNull()?.body ?: run { Log.w(TAG, "FD 다운로드 실패: ${r.code} ${r.reason}"); null }
    }

    /** SDS disposition 통지(TS 24.282 §12.2.1.1) — 대상 = 수신 SDS 의 보낸 사용자(mcdata-calling-user-id), 그룹 SDS 면 groupUri
     *  (mcdata-calling-group-id). 계정에 MCData PSI(ue-init-config)가 있으면 코어가 참여 기능 PSI 로 보낸다. */
    fun sendSdsNotification(peerId: String, convId: String, msgId: String, notifType: Int, groupUri: String = "") {
        c.cmd("sendSdsNotification") {
            c.account?.sendSdsNotification(peerId, convId, msgId, notifType, groupUri) ?: com.cims.ue.sdk.CimsResult.fail<Unit>(-1, "not registered")
        }
    }
}
