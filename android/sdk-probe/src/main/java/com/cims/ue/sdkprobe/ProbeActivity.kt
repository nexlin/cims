// SDK 엔진 점검 — 기기에서 :cimsue 코어가 도는지 한 번 훑는다(build.gradle.kts 머리말).
//
// 순서: 기동(카메라 CameraManager 주입) → 영상 장치 열거 → 장치 단 음량 → 라우트 → 캡처 게이트 끔/켬 → 재오픈 → 정지·해제.
// 각 단계의 결과와 엔진 로그를 화면·logcat(SdkProbe)에 남긴다. 판정은 사람이 아니라 줄로 한다 — `PASS`/`FAIL` 접두어.
package com.cims.ue.sdkprobe

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.widget.ScrollView
import android.widget.TextView
import com.cims.ue.sdk.AudioRoute
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.CimsUe
import com.cims.ue.sdk.EngineConfig
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class ProbeActivity : Activity() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private lateinit var out: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        out = TextView(this).apply { textSize = 12f; setPadding(24, 24, 24, 24); setTextIsSelectable(true) }
        setContentView(ScrollView(this).apply { addView(out) })
        scope.launch { run() }
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    private fun line(s: String) {
        Log.i(TAG, s)
        out.append(s + "\n")
    }

    private fun <T> check(what: String, r: CimsResult<T>): Boolean {
        line((if (r.ok) "PASS " else "FAIL ") + what + if (r.ok) "" else " — ${r.code} ${r.reason}")
        return r.ok
    }

    private suspend fun run() {
        val ue = CimsUe()
        val logJob = scope.launch { ue.log.collect { if (it.level <= 4) Log.i("$TAG-pj", it.message.trimEnd()) } }
        try {
            if (!check("start(context)", ue.start(EngineConfig(userAgent = "CIMS-SdkProbe/0.1", logLevel = 5), this))) return

            val vids = ue.videoDevices()
            vids.forEach { line("  video[${it.id}] ${it.driver} '${it.name}' capture=${it.capture} render=${it.render}") }
            val cams = vids.count { it.capture && it.driver.equals("Android", ignoreCase = true) }
            line((if (cams > 0) "PASS " else "FAIL ") + "camera enumerated: $cams (PjCamera2 via app class loader)")

            check("setDeviceAudioLevels(1.5, -26)", ue.setDeviceAudioLevels(1.5f, CimsUe.MIC_AGC_TARGET_DBOV))
            check("setAudioRoute(LOUDSPEAKER, EARPIECE)", ue.setAudioRoute(AudioRoute.LOUDSPEAKER, AudioRoute.EARPIECE))
            check("setCaptureEnabled(false)", ue.setCaptureEnabled(false))
            line((if (!ue.captureEnabled) "PASS " else "FAIL ") + "captureEnabled=false")
            check("setCaptureEnabled(true)", ue.setCaptureEnabled(true))
            check("reopenAudioDevice (closed = no-op)", ue.reopenAudioDevice())
            check("setVideoSurface(null)", ue.setVideoSurface(null))
            ue.stop()
            line("PASS stop")
        } catch (t: Throwable) {
            line("FAIL exception ${t.javaClass.simpleName}: ${t.message}")
        } finally {
            logJob.cancel()
            withContext(Dispatchers.IO) { ue.close() }
            line("DONE")
        }
    }

    private companion object {
        const val TAG = "SdkProbe"
    }
}
