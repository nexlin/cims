package com.cims.ue.ptt

import android.content.Context
import com.cims.ue.core.account.CimsLogoutReceiver

/** CIMS 로그아웃 통지 — 프로비저닝 설정 제거(베이스) 후 등록 해제 + 등록유지 FGS 종료. */
class SuiteLogoutReceiver : CimsLogoutReceiver() {
    override fun onLogout(context: Context) {
        PttService.instance?.stopSip()
    }
}
