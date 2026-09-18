package com.cims.ue.core.power

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings

/**
 * 배터리 최적화(Doze) 예외 요청 — CIMS 앱은 절전 규칙의 영향을 받지 않아야 한다.
 *
 * Doze 는 앱의 망을 막고(netd), 알람을 유예하고, wakelock 을 무시한다. 예외 목록에 들면 이 셋을
 * 면제받는다. **CPU 를 깨워 두는 것은 아니다** — 그건 [PartialWakeLock] 의 몫이고, Doze 가 wakelock 을
 * 무시하지 않게 하려면 예외가 선행돼야 하므로 둘은 세트다.
 *
 * 오너앱(cims)이 특히 중요하다 — 토큰 갱신(인증기)이 그 프로세스에서 돌아, 오너앱이 예외가 아니면
 * 호출 앱(PTT/VoLTE)이 예외여도 대기모드에서 갱신이 연결 타임아웃으로 실패한다(실측 09-17,
 * android_ue_provisioning.md §5).
 *
 * 요청은 시스템 동의 다이얼로그(ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)를 띄우는 것이고, 승인 여부는
 * 사용자가 정한다. 거부되면 다음 진입 때 다시 묻되, 한 프로세스 안에서는 한 번만 묻는다 — 같은 세션에서
 * 되묻는 고리를 만들지 않는다. 매니페스트에 `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` 선언이 필요하다.
 */
object BatteryExemption {

    /** 이 프로세스에서 이미 물은 패키지 — 같은 세션에서 되묻는 고리를 만들지 않는다. */
    private val askedThisProcess = java.util.Collections.synchronizedSet(HashSet<String>())

    /** [pkg](기본 = 자기 앱)가 배터리 최적화 예외 목록에 있는가. */
    fun isExempt(context: Context, pkg: String = context.packageName): Boolean {
        val pm = context.getSystemService(PowerManager::class.java) ?: return false
        return pm.isIgnoringBatteryOptimizations(pkg)
    }

    /**
     * [pkg](기본 = 자기 앱)가 예외가 아니면 시스템 동의 다이얼로그를 띄운다. 프로세스당 패키지마다 한 번.
     * 설정 앱은 URI 의 패키지를 그대로 다이얼로그에 싣는다 — 다른 앱을 대신해 물을 수 있다(실측 09-18,
     * W999: `package:com.cims.ue.cims` → "CIMS이(가) 항상 백그라운드에서 실행되도록 허용하시겠습니까?").
     * @return 다이얼로그를 띄웠으면 true.
     */
    fun requestOnce(context: Context, pkg: String = context.packageName): Boolean {
        if (pkg in askedThisProcess || isExempt(context, pkg)) return false
        askedThisProcess.add(pkg)
        return runCatching {
            context.startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                    .setData(Uri.parse("package:$pkg"))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            )
            true
        }.getOrDefault(false)
    }

    /**
     * 동반 앱(PTT/VoLTE)의 진입 훅 — 자기 앱과 **오너앱(cims)** 의 예외를 차례로 확보한다.
     *
     * 오너앱이 중요한 이유: 토큰 갱신(인증기)이 그 프로세스에서 돌아, 오너앱이 예외가 아니면 동반 앱이
     * 예외여도 대기모드에서 갱신이 망 차단에 걸린다. 오너앱 자체는 로그인 화면 말고는 진입점이 없어
     * 이미 로그인된 단말에서는 스스로 물을 기회가 없다 — 그래서 동반 앱이 대신 묻는다.
     *
     * 한 진입에 다이얼로그는 하나만 — 자기 앱이 아직 예외가 아니면 그것만 묻고, 오너앱은 다음 진입에
     * 묻는다(둘을 겹쳐 띄우지 않는다).
     * @return 다이얼로그를 띄웠으면 true.
     */
    fun requestOnceWithOwner(context: Context): Boolean {
        if (requestOnce(context)) return true
        return requestOnce(context, com.cims.ue.core.CimsSuite.CIMS_PACKAGE)
    }
}
