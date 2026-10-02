// 설정 [영상] — MCVideo 영상 채널 (android_dispatch_tablet.md §6.14, dispatch_desktop_ui.md §10.3)
//
// 둘뿐이다: [영상 보내기] 가 쓸 **카메라**(앞/뒤)와 **«영상 보내는 중 무전»**(D12 — 마이크 경합을 누구에게 줄지). 다른 설정과 같이
// 바꾸면 곧바로 저장되고 적용된다 — 카메라는 보내는 중이면 그 자리에서 바뀐다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.cims.ue.dispatch.session.DispatchSession
import com.cims.ue.dispatch.session.VideoCamera
import com.cims.ue.dispatch.session.VideoMicPolicy
import com.cims.ue.dispatch.session.setVideoCamera
import com.cims.ue.dispatch.session.videoCameras

/** 엔진 카메라 이름(pjmedia `Front camera`·`Back camera`) → 설정값. 이름으로 가를 수 없으면 null. */
internal fun videoCameraOf(name: String): String? = when {
    name.contains("front", ignoreCase = true) -> VideoCamera.FRONT
    name.contains("back", ignoreCase = true) -> VideoCamera.BACK
    else -> null
}

/** 설정 [영상] 의 항목들 — **세션을 붙이는 껍데기**. */
@Composable
internal fun VideoSettings(session: DispatchSession) {
    val s by session.settingsFlow.collectAsStateWithLifecycle()
    val cameras by session.videoCameras.collectAsStateWithLifecycle()
    VideoSettingsContent(
        camera = s.videoCamera, micPolicy = s.videoMicPolicy,
        available = cameras.mapNotNull(::videoCameraOf).toSet(), anyCamera = cameras.isNotEmpty(),
        onCamera = session::setVideoCamera,
        onMicPolicy = { v -> session.updateSettings { it.copy(videoMicPolicy = v) } })
}

/**
 * 설정 [영상] 본문 — **순수 컴포저블**.
 *
 * @param available 기기에 있는 카메라(`front`·`back`) — 없는 쪽은 흐리다
 * @param anyCamera 엔진이 카메라를 하나라도 아는가 — 없으면 [영상 보내기] 가 꺼져 있다(영상 보기는 된다)
 */
@Composable
internal fun VideoSettingsContent(
    camera: String,
    micPolicy: String,
    available: Set<String> = setOf(VideoCamera.FRONT, VideoCamera.BACK),
    anyCamera: Boolean = true,
    onCamera: (String) -> Unit = {},
    onMicPolicy: (String) -> Unit = {},
) {
    val soft = MaterialTheme.colorScheme.onSurfaceVariant
    Text("영상 보내기 카메라", fontSize = Type.body, color = soft)
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        listOf(VideoCamera.FRONT to "앞 카메라", VideoCamera.BACK to "뒤 카메라").forEach { (v, label) ->
            CimsFilterChip(selected = camera == v, onClick = { onCamera(v) }, enabled = v in available,
                label = { Text(label, fontSize = Type.meta) })
        }
    }
    Text(
        if (anyCamera) "보내는 중에 바꾸면 곧바로 바뀐다. 카메라는 로그인할 때 한 번 찾는다."
        else "이 기기에서 카메라를 찾지 못했습니다 — [영상 보내기] 가 꺼져 있습니다(영상 보기는 됩니다).",
        fontSize = Type.meta, color = soft, modifier = Modifier.padding(top = 2.dp, bottom = 6.dp))

    Text("영상 보내는 중 무전", fontSize = Type.body, color = soft)
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        listOf(VideoMicPolicy.VOICE to "음성 우선", VideoMicPolicy.VIDEO to "영상 우선").forEach { (v, label) ->
            CimsFilterChip(selected = micPolicy == v, onClick = { onMicPolicy(v) }, label = { Text(label, fontSize = Type.meta) })
        }
    }
    Text(
        if (micPolicy == VideoMicPolicy.VIDEO)
            "영상을 보내는 동안 PTT 는 무전 발언을 요청하지 않는다. 긴급·임박 채널은 그대로 말할 수 있다."
        else "PTT 를 누르는 동안 마이크는 음성 무전으로 가고 영상 호 소리만 멈춘다(영상은 계속).",
        fontSize = Type.meta, color = soft, modifier = Modifier.padding(top = 2.dp, bottom = 4.dp))
}
