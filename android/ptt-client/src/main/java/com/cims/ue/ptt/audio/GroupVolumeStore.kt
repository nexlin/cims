package com.cims.ue.ptt.audio

import android.content.Context

/**
 * 그룹(채널)별 수신 음량 영속화 — SharedPreferences(groupId → 0~2f).
 * 저장값이 없는(새로 참여/수신한) 그룹은 **최대 음량**이 기본이며, 폰 리부팅·앱 재기동에도 유지된다.
 */
class GroupVolumeStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("group_volume", Context.MODE_PRIVATE)

    fun get(groupId: String): Float =
        prefs.getFloat(groupId, DEFAULT).coerceIn(0f, MAX)

    fun set(groupId: String, level: Float) =
        prefs.edit().putFloat(groupId, level.coerceIn(0f, MAX)).apply()

    companion object {
        /** 수신 음량 최대(200% — conference bridge RxLevel 증폭 상한, 슬라이더 상한과 동일). */
        const val MAX = 2f
        /** 신규 그룹 기본 = 최대. */
        const val DEFAULT = MAX
    }
}
