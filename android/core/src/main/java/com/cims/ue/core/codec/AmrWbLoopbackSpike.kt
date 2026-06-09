package com.cims.ue.core.codec

import android.media.MediaCodec
import android.media.MediaFormat
import kotlin.math.sin

/**
 * AMR-WB MediaCodec 루프백 스파이크 (M0 최대 리스크 검증).
 *
 * 합성 PCM(사인파)을 20ms 프레임 단위로 AMR-WB 인코더 → 디코더에 통과시켜
 * 처리량을 측정한다. 핵심 판정: **인코드+디코드 총 시간이 실시간 예산(프레임수 × 20ms)
 * 보다 충분히 작은가** (코덱이 실시간 음성의 병목이 아닌가).
 *
 * 주의: 이는 코덱 처리량/가용성 검증이며, 실제 mouth-to-ear 지연(오디오 캡처/재생 +
 * 네트워크 + 지터버퍼)과는 다르다. 전체 지연은 M1(PJSIP 연동) 에서 별도 측정한다.
 */
class AmrWbLoopbackSpike {

    data class Result(
        val ok: Boolean,
        val message: String,
        val frames: Int = 0,
        val encoderName: String? = null,
        val decoderName: String? = null,
        val encTotalMs: Double = 0.0,
        val decTotalMs: Double = 0.0,
        val realtimeBudgetMs: Double = 0.0,
        val encodedBytes: Int = 0,
        val decodedFrames: Int = 0,
    ) {
        val encPerFrameMs: Double get() = if (frames > 0) encTotalMs / frames else 0.0
        val decPerFrameMs: Double get() = if (frames > 0) decTotalMs / frames else 0.0
        val sustainsRealtime: Boolean get() = ok && (encTotalMs + decTotalMs) < realtimeBudgetMs

        fun report(): String = buildString {
            appendLine("=== AMR-WB MediaCodec 루프백 스파이크 ===")
            appendLine(message)
            if (!ok) return@buildString
            appendLine("프레임: $frames (각 20ms) ≈ ${"%.1f".format(realtimeBudgetMs / 1000)}s 분량")
            appendLine("encoder: $encoderName")
            appendLine("decoder: $decoderName")
            appendLine("인코딩 총 ${"%.1f".format(encTotalMs)}ms (프레임당 ${"%.3f".format(encPerFrameMs)}ms)")
            appendLine("디코딩 총 ${"%.1f".format(decTotalMs)}ms (프레임당 ${"%.3f".format(decPerFrameMs)}ms)")
            appendLine("실시간 예산: ${"%.1f".format(realtimeBudgetMs)}ms")
            appendLine("인코딩 산출: ${encodedBytes}B, 디코딩 프레임: $decodedFrames")
            appendLine()
            appendLine(
                if (sustainsRealtime) "✅ 실시간 처리량 충족 (enc+dec < 예산)"
                else "⚠️ 실시간 예산 초과 — 추가 검토/폴백 필요"
            )
            appendLine()
            appendLine("※ 처리량 검증용 수치. 실제 mouth-to-ear 지연은 M1(PJSIP 연동)에서 측정.")
        }
    }

    /** @param frames 20ms 프레임 수 (기본 250 ≈ 5초) */
    fun run(frames: Int = 250): Result {
        val mime = MediaCodecCapabilities.MIME_AMR_WB
        val sampleRate = 16000
        val channels = 1
        val frameSamples = 320          // 20ms @ 16kHz
        val frameBytes = frameSamples * 2
        val bitRate = 23850             // AMR-WB mode 8. 운영 시 SDP mode-set 정합 필요.

        // 1) 합성 PCM16(440Hz 사인파) 입력 프레임
        val pcmFrames = ArrayList<ByteArray>(frames)
        var phase = 0.0
        val dPhase = 2.0 * Math.PI * 440.0 / sampleRate
        for (f in 0 until frames) {
            val b = ByteArray(frameBytes)
            for (i in 0 until frameSamples) {
                val s = (sin(phase) * 8000).toInt()
                b[i * 2] = (s and 0xff).toByte()
                b[i * 2 + 1] = ((s shr 8) and 0xff).toByte()
                phase += dPhase
            }
            pcmFrames.add(b)
        }

        var enc: MediaCodec? = null
        var dec: MediaCodec? = null
        try {
            // 2) 인코더
            enc = try {
                MediaCodec.createEncoderByType(mime)
            } catch (e: Exception) {
                return Result(false, "AMR-WB 인코더 생성 실패: ${e.message}\n(이 기기에 AMR-WB 인코더가 없을 수 있음 — 코덱 가용성 먼저 확인)")
            }
            val encFmt = MediaFormat.createAudioFormat(mime, sampleRate, channels).apply {
                setInteger(MediaFormat.KEY_BIT_RATE, bitRate)
            }
            enc.configure(encFmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            enc.start()
            val encName = enc.name

            val t0 = System.nanoTime()
            val encoded = pump(enc, pcmFrames)
            val encMs = (System.nanoTime() - t0) / 1e6

            // 3) 디코더
            dec = try {
                MediaCodec.createDecoderByType(mime)
            } catch (e: Exception) {
                return Result(false, "AMR-WB 디코더 생성 실패: ${e.message}")
            }
            val decFmt = MediaFormat.createAudioFormat(mime, sampleRate, channels)
            dec.configure(decFmt, null, null, 0)
            dec.start()
            val decName = dec.name

            val t1 = System.nanoTime()
            val decoded = pump(dec, encoded)
            val decMs = (System.nanoTime() - t1) / 1e6

            return Result(
                ok = true,
                message = "성공",
                frames = frames,
                encoderName = encName,
                decoderName = decName,
                encTotalMs = encMs,
                decTotalMs = decMs,
                realtimeBudgetMs = frames * 20.0,
                encodedBytes = encoded.sumOf { it.size },
                decodedFrames = decoded.size,
            )
        } catch (e: Exception) {
            return Result(false, "스파이크 예외: $e")
        } finally {
            runCatching { enc?.stop() }
            runCatching { enc?.release() }
            runCatching { dec?.stop() }
            runCatching { dec?.release() }
        }
    }

    /** 동기 버퍼 API로 입력 프레임들을 코덱에 통과시켜 출력 프레임 목록을 반환 */
    private fun pump(codec: MediaCodec, inputs: List<ByteArray>): List<ByteArray> {
        val outputs = ArrayList<ByteArray>()
        val info = MediaCodec.BufferInfo()
        var inIdx = 0
        var ptsUs = 0L
        var inEos = false
        var outEos = false
        val timeoutUs = 15_000L
        while (!outEos) {
            if (!inEos) {
                val ib = codec.dequeueInputBuffer(timeoutUs)
                if (ib >= 0) {
                    if (inIdx < inputs.size) {
                        val buf = codec.getInputBuffer(ib)!!
                        buf.clear()
                        buf.put(inputs[inIdx])
                        codec.queueInputBuffer(ib, 0, inputs[inIdx].size, ptsUs, 0)
                        ptsUs += 20_000
                        inIdx++
                    } else {
                        codec.queueInputBuffer(ib, 0, 0, ptsUs, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                        inEos = true
                    }
                }
            }
            val ob = codec.dequeueOutputBuffer(info, timeoutUs)
            if (ob >= 0) {
                val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                if (info.size > 0 && !isConfig) {
                    val out = ByteArray(info.size)
                    val obuf = codec.getOutputBuffer(ob)!!
                    obuf.position(info.offset)
                    obuf.get(out, 0, info.size)
                    outputs.add(out)
                }
                codec.releaseOutputBuffer(ob, false)
                if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outEos = true
            }
        }
        return outputs
    }
}
