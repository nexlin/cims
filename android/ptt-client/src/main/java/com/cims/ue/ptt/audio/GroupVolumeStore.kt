package com.cims.ue.ptt.audio

import android.content.Context

/**
 * 그룹(채널)별 수신 음량 영속화 — SharedPreferences(groupId → 0~2f).
 * 저장값이 없는(새로 참여/수신한) 그룹은 **최대 음량**이 기본이며, 폰 리부팅·앱 재기동에도 유지된다.
 * 풀스케일 초과는 엔진 리미터가 막는다(ue_audio_level.md §3).
 */
class GroupVolumeStore(context: Context) {

    private val prefs = context.applicationContext
        .getSharedPreferences("group_volume", Context.MODE_PRIVATE)
        .also { p ->
            // 배선 v1 은 이 값이 **보내는** 크기(내 음성)에 걸려 있었다(ue_audio_level.md §2) — 그 위에서
            // 맞춘 값은 수신 음량으로서 의미가 없으므로 한 번 비우고 기본값에서 다시 시작한다.
            if (p.getInt(WIRING_KEY, 1) < WIRING) p.edit().clear().putInt(WIRING_KEY, WIRING).apply()
        }

    fun get(groupId: String): Float =
        prefs.getFloat(groupId, DEFAULT).coerceIn(0f, MAX)

    fun set(groupId: String, level: Float) =
        prefs.edit().putFloat(groupId, level.coerceIn(0f, MAX)).apply()

    companion object {
        /** 수신 음량 최대(200% — conference bridge 유입 배율 상한, 슬라이더 상한과 동일). */
        const val MAX = 2f
        /** 신규 그룹 기본 = 최대. */
        const val DEFAULT = MAX
        /** 배선 판 기록 키 — groupId 와 겹치지 않는 이름. */
        private const val WIRING_KEY = "~wiring"
        private const val WIRING = 2
    }
}
