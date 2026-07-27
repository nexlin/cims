package com.cims.ue.ptt

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Bundle
import android.os.IBinder
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import com.cims.ue.ptt.ui.AppRoot
import com.cims.ue.ptt.ui.PttTheme

class MainActivity : ComponentActivity() {
    private var svc by mutableStateOf<PttService?>(null)
    private val conn = object : ServiceConnection {
        override fun onServiceConnected(n: ComponentName?, b: IBinder?) { svc = (b as? PttService.LocalBinder)?.service }
        override fun onServiceDisconnected(n: ComponentName?) { svc = null }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 하드웨어 음량 키가 무전 재생 스트림을 조절하게 — 재생 트랙은 STREAM_MUSIC
        // (트랙 단위 분리 라우팅용 pjsip 패치: 통화와 무전을 서로 다른 출력으로)
        volumeControlStream = android.media.AudioManager.STREAM_MUSIC
        HwPtt.init(this)
        PttService.start(this)
        bindService(Intent(this, PttService::class.java), conn, Context.BIND_AUTO_CREATE)
        setContent {
            PttTheme {
                Surface(Modifier.fillMaxSize()) {
                    AppRoot(svc, onStopSip = { svc?.stopSip(); svc = null })
                }
            }
        }
    }

    override fun onDestroy() {
        runCatching { unbindService(conn) }
        super.onDestroy()
    }

    /** 하드웨어 PTT 키(측면 버튼) — down=발언 요청, up=해제. 화면 버튼과 동일 경로.
     *  SOS(2번째 측면 키)=긴급 개시 — 해제는 오조작 방지 위해 화면 배너에서만.
     *  버튼 학습 모드면 눌린 keycode 를 대상 버튼으로 저장(HwPtt.consumeLearn).
     *  접근성 키 필터(PttKeyService) 활성 시엔 키가 거기서 소비돼 여기 오지 않는다 —
     *  이 경로는 접근성 미활성(전면 전용) 폴백. */
    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        // 키코드 실측용 (persist.log.tag=I 단말이 있어 Log.i)
        if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0)
            android.util.Log.i("HwPtt", "keyDown code=${event.keyCode} scan=${event.scanCode} dev=${event.deviceId}")
        // 버튼 학습: DOWN 에서 캡처, UP 은 소비만(정상 dispatch 로 새지 않게)
        if (HwPtt.learning.value != null) {
            if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                HwPtt.consumeLearn(this, event.keyCode)
            if (!HwPtt.isSystemNav(event.keyCode)) return true
        }
        when (HwPtt.classify(event.keyCode)) {
            HwPtt.Kind.SOS -> {
                HwPtt.noteKeyEvent()
                if (event.action == android.view.KeyEvent.ACTION_DOWN && event.repeatCount == 0)
                    svc?.controller?.startEmergency()
                return true
            }
            HwPtt.Kind.PTT -> {
                HwPtt.noteKeyEvent()
                HwPtt.markSeen(this)
                when (event.action) {
                    android.view.KeyEvent.ACTION_DOWN -> if (event.repeatCount == 0) svc?.controller?.pttDown()
                    android.view.KeyEvent.ACTION_UP -> svc?.controller?.pttUp()
                }
                return true
            }
            HwPtt.Kind.NONE -> Unit
        }
        return super.dispatchKeyEvent(event)
    }
}
