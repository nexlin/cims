package com.cims.ue.core.account

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import com.cims.ue.core.CimsSuite
import com.cims.ue.core.config.ConfigStore

/**
 * CIMS 오너 앱 로그아웃 통지([CimsSuite.ACTION_LOGOUT]) 수신 — companion 앱(Phone/PTT)의
 * 로그인 세션 정리. 매니페스트 정적 등록(서명 권한 보호)이라 프로세스가 죽어 있어도 배달돼
 * 캐시된 프로비저닝 설정이 항상 제거된다(다음 기동 시 stale 자격증명으로 재등록 방지).
 *
 * 각 앱은 서브클래싱해 [onLogout] 에서 실행 중 서비스 종료(등록 해제 + FGS 정리)를 수행한다.
 * 수동 설정 모드는 CIMS 계정과 무관하게 동작하므로 로그아웃 대상이 아니다.
 *
 * 마지막에 **프로세스를 종료**한다(un-REGISTER 송신 여유 후) — 로그아웃 계약이 "앱 종료"이기도
 * 하고, PJSIP 은 프로세스 내 재부팅(libDestroy 후 Endpoint 재생성)이 취약해(네이티브 Endpoint
 * 싱글톤·log writer 수명 — pj_thread_this abort 실측) 다음 로그인이 항상 신규 프로세스의
 * 첫 부팅 경로를 타게 하는 것이 결정적이다.
 */
abstract class CimsLogoutReceiver : BroadcastReceiver() {

    /** 실행 중 리소스 정리 — 등록 해제·FGS 종료. 설정 제거 후 호출된다. */
    abstract fun onLogout(context: Context)

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != CimsSuite.ACTION_LOGOUT) return
        val store = ConfigStore(context)
        if (store.isManual()) return
        store.clear()
        onLogout(context)
        Handler(Looper.getMainLooper()).postDelayed(
            { android.os.Process.killProcess(android.os.Process.myPid()) },
            EXIT_DELAY_MS,
        )
    }

    private companion object {
        /** 종료 전 대기 — stopSip 의 un-REGISTER(pj-ctl 비동기 + UDP 송신)가 나갈 시간. */
        const val EXIT_DELAY_MS = 2_000L
    }
}
