package com.cims.ue.core.codec

import android.media.MediaCodecList
import android.os.Build

/**
 * 단말의 MediaCodec 코덱 가용성 조회.
 *
 * M0 목적: UNIWA(또는 임의 단말)에서 AMR-WB(음성)·H.264(영상) 인코더/디코더가
 * 실제로 존재하는지, SW/HW 가속 여부를 확인한다. (코덱 부재 = 설계 재검토 신호)
 */
object MediaCodecCapabilities {

    const val MIME_AMR_WB = "audio/amr-wb"
    const val MIME_H264 = "video/avc"

    data class CodecEntry(
        val name: String,
        val isEncoder: Boolean,
        val supportedTypes: List<String>,
        val hardwareAccelerated: Boolean?,  // API 29+
        val vendor: Boolean?,               // API 29+
    )

    fun query(mimeFilter: String? = null): List<CodecEntry> {
        val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
        return infos
            .filter { info ->
                mimeFilter == null || info.supportedTypes.any { it.equals(mimeFilter, ignoreCase = true) }
            }
            .map { info ->
                CodecEntry(
                    name = info.name,
                    isEncoder = info.isEncoder,
                    supportedTypes = info.supportedTypes.toList(),
                    hardwareAccelerated = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) info.isHardwareAccelerated else null,
                    vendor = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) info.isVendor else null,
                )
            }
    }

    /** 사람이 읽을 요약 문자열 (스파이크 화면 표시용) */
    fun summary(): String = buildString {
        appendLine("=== MediaCodec 가용성 ===")
        appendLine("기기: ${Build.MANUFACTURER} ${Build.MODEL}")
        appendLine("Android ${Build.VERSION.RELEASE} (SDK ${Build.VERSION.SDK_INT})")

        for ((label, mime) in listOf("AMR-WB (음성)" to MIME_AMR_WB, "H.264 (영상)" to MIME_H264)) {
            appendLine()
            appendLine("[$label] $mime")
            val entries = query(mime)
            if (entries.isEmpty()) {
                appendLine("  ⚠️ 지원 코덱 없음")
            } else {
                for (e in entries) {
                    val kind = if (e.isEncoder) "ENC" else "DEC"
                    val hw = when (e.hardwareAccelerated) {
                        true -> "HW"; false -> "SW"; null -> "?"
                    }
                    appendLine("  - [$kind/$hw] ${e.name}")
                }
            }
        }
    }
}
