// 녹취 세그먼트 재생 — MediaPlayer 한 개 (docs/design/features/android_dispatch_tablet.md §6.5)
//
// 얇게 둔다: 파일 하나를 틀고, 끝나면 알리고, 멈춘다. 라우트·음량은 AudioRouter 소관이고 여기서는
// 통화·무전과 섞이지 않게 **미디어 스트림**으로만 낸다. 시험에서는 이 클래스를 갈아 끼운다(open).
package com.cims.ue.dispatch.ui.history

import android.media.AudioAttributes
import android.media.MediaPlayer
import java.io.File

open class SegmentPlayer {

    private var mp: MediaPlayer? = null

    /** 재생 시작. 성공하면 true, 끝나면 [onDone]. 이미 틀고 있던 것은 멈춘다. */
    open fun play(file: File, onDone: () -> Unit): Boolean {
        stop()
        return try {
            val p = MediaPlayer().apply {
                setAudioAttributes(AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build())
                setDataSource(file.absolutePath)
                setOnCompletionListener { onDone() }
                setOnErrorListener { _, _, _ -> onDone(); true }
                prepare()
                start()
            }
            mp = p
            true
        } catch (_: Exception) {
            // 변환이 덜 끝났거나 파일이 깨졌다 — 호출자가 문구를 낸다.
            release()
            false
        }
    }

    open fun stop() {
        mp?.let { runCatching { it.stop() }; runCatching { it.release() } }
        mp = null
    }

    open fun release() = stop()
}
