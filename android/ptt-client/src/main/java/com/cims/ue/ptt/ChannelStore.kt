package com.cims.ue.ptt

import android.content.Context

/**
 * 참여 채널 영속화 — 프로세스 재시작(강제종료·재설치·리부팅) 후 자동 재조인용.
 * joined = 참여 의도가 있는 그룹(참여 순서 유지), primary = 주채널, sessionUri = 지금 참가한 세션의 식별자.
 * "의도" 저장소라 서버/네트워크 사정으로 세션이 끊겨도 지우지 않으며, 사용자가 명시적으로
 * 나가면([com.cims.ue.ptt.PttController.leaveGroup]) 제거한다. 로그아웃 시에는
 * [SuiteLogoutReceiver] 가 [clear] 를 호출한다(다른 사용자 재로그인 대비).
 */
class ChannelStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("ptt_channels", Context.MODE_PRIVATE)

    var primary: String?
        get() = prefs.getString(K_PRIMARY, null)?.takeIf { it.isNotBlank() }
        set(v) {
            val e = prefs.edit().putString(K_PRIMARY, v ?: "")
            // 마지막 주채널은 별도 보존 — 참여 이탈로 primary 가 비워져도 SOS "선택 그룹"
            // (TS 24.484 UseCurrentlySelectedGroup)의 대상으로 남는다. clear(로그아웃)로만 소거.
            if (!v.isNullOrBlank()) e.putString(K_LAST_PRIMARY, v)
            e.apply()
        }

    /** 마지막 주채널 (SOS 선택그룹 폴백) — primary 해제/이탈 후에도 유지. */
    val lastPrimary: String?
        get() = prefs.getString(K_LAST_PRIMARY, null)?.takeIf { it.isNotBlank() }

    /** 참여 그룹(참여 순서 유지). groupId 에 쉼표가 없다는 전제의 CSV 저장. */
    var joined: List<String>
        get() = prefs.getString(K_JOINED, "").orEmpty().split(',').filter { it.isNotBlank() }
        set(v) { prefs.edit().putString(K_JOINED, v.distinct().joinToString(",")).apply() }

    fun add(groupId: String) { joined = joined + groupId }

    fun remove(groupId: String) {
        joined = joined - groupId
        if (primary == groupId) primary = null
        setSessionUri(groupId, null)
    }

    /** 지금 참가해 있는 그룹 세션의 식별자(TS 24.379 §4.5 — 제어 기능 Contact 의 GRUU). 세션이 끝나면 지운다 — 프로세스가 세션 중에
     *  죽었을 때만 남아, 다시 섰을 때 그 세션에 재합류하는 근거가 된다(§10.1.1.2.4.1). */
    fun sessionUri(groupId: String): String? = prefs.getString(K_SESSION + groupId, null)?.takeIf { it.isNotBlank() }

    fun setSessionUri(groupId: String, uri: String?) {
        val e = prefs.edit()
        if (uri.isNullOrBlank()) e.remove(K_SESSION + groupId) else e.putString(K_SESSION + groupId, uri)
        e.apply()
    }

    fun clear() = prefs.edit().clear().apply()

    private companion object {
        const val K_PRIMARY = "primary"
        const val K_LAST_PRIMARY = "last_primary"
        const val K_JOINED = "joined"
        const val K_SESSION = "session:"
    }
}
