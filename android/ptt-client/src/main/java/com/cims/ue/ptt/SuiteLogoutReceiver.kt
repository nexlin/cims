package com.cims.ue.ptt

import android.content.Context
import com.cims.ue.core.account.CimsLogoutReceiver

/** CIMS 로그아웃 통지 — 프로비저닝 설정 제거(베이스) 후 등록 해제 + 등록유지 FGS 종료.
 *  참여 채널 영속([ChannelStore])도 제거 — 다른 사용자 재로그인 시 이전 사용자의 채널로
 *  자동 재조인하는 사고 방지(같은 사용자 재로그인은 서버 fan-out INVITE 로 복원됨). */
class SuiteLogoutReceiver : CimsLogoutReceiver() {
    override fun onLogout(context: Context) {
        ChannelStore(context).clear()
        PttService.instance?.stopSip()
    }
}
