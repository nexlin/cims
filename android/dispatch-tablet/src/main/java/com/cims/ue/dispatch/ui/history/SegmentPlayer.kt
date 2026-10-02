// 녹취 세그먼트 재생 — MediaPlayer 한 개 (docs/design/features/android_dispatch_tablet.md §6.11)
//
// 얇게 둔다: 파일 하나를 그 안의 한 지점에서 열고, 옮기고, 멈추고, 끝나면 알린다. «어느 세그먼트의 어디» 를 고르는 일은
// [PlayerBar] 와 VM 의 몫이다. 라우트·음량은 AudioRouter 소관이고 여기서는 통화·무전과 섞이지 않게 **미디어 스트림**으로만
// 낸다. 영상이 있는 세그먼트(영상 통화 녹취·MCVideo 송출 구간)는 같은 MP4 의 영상 트랙을 [setSurface] 로 받은 면에 그린다 —
// 면이 없으면 소리만 난다. 시험에서는 이 클래스를 갈아 끼운다(open).
package com.cims.ue.dispatch.ui.history

import android.media.AudioAttributes
import android.media.MediaPlayer
import android.view.Surface
import java.io.File

open class SegmentPlayer {

    private var mp: MediaPlayer? = null
    /** 영상 칸의 그리기 면 — 칸이 열려 있는 동안만 있다. 재생기를 새로 만들 때마다 다시 붙인다. */
    private var surface: Surface? = null

    /** 파일이 열려 있다(재생 중이거나 멈춰 있다). */
    open val isOpen: Boolean get() = mp != null

    /** 파일 안 위치(ms). 열린 것이 없으면 0. */
    open val positionMs: Int get() = mp?.let { runCatching { it.currentPosition }.getOrDefault(0) } ?: 0

    /**
     * 파일을 [offsetMs] 에서 연다. [play] 면 바로 틀고, 아니면 그 자리에 멈춘 채 둔다. 이미 열려 있던 것은 닫는다.
     * 성공하면 true. 끝까지 틀면 [onDone], 재생 중 깨지면 [onError], 영상 크기를 알게 되면 [onVideoSize].
     */
    open fun open(
        file: File,
        offsetMs: Int,
        play: Boolean,
        speed: Float,
        onDone: () -> Unit,
        onError: () -> Unit = onDone,
        onVideoSize: (Int, Int) -> Unit = { _, _ -> },
    ): Boolean {
        stop()
        val p = MediaPlayer()
        mp = p
        return try {
            p.setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build())
            p.setDataSource(file.absolutePath)
            surface?.takeIf { it.isValid }?.let(p::setSurface)
            p.setOnVideoSizeChangedListener { _, w, h -> if (w > 0 && h > 0) onVideoSize(w, h) }
            p.setOnCompletionListener { onDone() }
            p.setOnErrorListener { _, _, _ -> onError(); true }
            p.prepare()
            if (offsetMs > 0) p.seekTo(offsetMs.toLong(), MediaPlayer.SEEK_CLOSEST)
            if (play) { p.start(); applySpeed(p, speed) }
            true
        } catch (_: Exception) {
            // 변환이 덜 끝났거나 파일이 깨졌다 — 호출자가 문구를 낸다.
            stop()
            false
        }
    }

    /** 같은 파일 안에서 위치만 옮긴다. */
    open fun seekTo(ms: Int) {
        mp?.let { p -> runCatching { p.seekTo(ms.coerceAtLeast(0).toLong(), MediaPlayer.SEEK_CLOSEST) } }
    }

    open fun pause() {
        mp?.let { p -> runCatching { if (p.isPlaying) p.pause() } }
    }

    /** 멈춘 자리에서 이어 튼다. */
    open fun resume(speed: Float) {
        mp?.let { p -> runCatching { p.start(); applySpeed(p, speed) } }
    }

    /**
     * 재생 속도. **틀고 있을 때만** 건다 — 멈춘 재생기에 0 이 아닌 속도를 주면 MediaPlayer 가 재생을 시작한다. 멈춰 있는 동안
     * 바꾼 값은 [resume]·[open] 이 받아 건다.
     */
    open fun setSpeed(speed: Float) {
        mp?.let { p -> if (runCatching { p.isPlaying }.getOrDefault(false)) applySpeed(p, speed) }
    }

    /** 영상 칸의 그리기 면을 붙이거나(칸이 열렸다) 뗀다(null — 칸이 닫혔다). 재생 중이어도 된다. */
    open fun setSurface(s: Surface?) {
        surface = s
        mp?.let { p -> runCatching { p.setSurface(s?.takeIf { it.isValid }) } }
    }

    private fun applySpeed(p: MediaPlayer, speed: Float) {
        runCatching { p.playbackParams = p.playbackParams.setSpeed(speed) }
    }

    /** 파일을 닫는다. 그리기 면은 그대로 둔다 — 다음 세그먼트가 이어 쓴다. */
    open fun stop() {
        mp?.let { runCatching { it.stop() }; runCatching { it.release() } }
        mp = null
    }

    /** MediaPlayer 는 스스로 안 사라진다 — 소유자가 닫을 때 부른다. */
    open fun release() {
        stop()
        surface = null
    }
}
