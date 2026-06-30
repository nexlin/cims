package com.cims.ue.volte

import android.content.Context
import android.content.Intent
import com.cims.ue.core.boot.CimsBootReceiver

/** 부팅 시 공유 계정이 있으면 SipService 를 시작(무인 자동 로그인 → REGISTER). */
class BootReceiver : CimsBootReceiver() {
    override fun serviceIntent(context: Context): Intent = Intent(context, SipService::class.java)
}
