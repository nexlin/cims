// CSV 내보내기 — 저장 위치는 사람이 고른다 (android_dispatch_tablet.md §6.2d·§6.7, dispatch_desktop_ui.md §4.4)
//
// 데스크톱은 저장 대화상자(`SaveFileDialog`)다. 태블릿은 시스템 문서 선택기(`CreateDocument`)로 같은 일을 한다 — 앱이
// 저장소 권한을 들지 않고, 사람이 고른 자리(내부 저장소·USB·클라우드)에 쓴다. 내용은 **누른 순간의 목록**이다.
package com.cims.ue.dispatch.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import com.cims.ue.dispatch.session.DispatchService
import com.cims.ue.dispatch.session.NoticeLevel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** UTF-8 BOM — 엑셀이 한글 CSV 를 깨지 않게(데스크톱 `new UTF8Encoding(true)` 와 같다). */
private val BOM = byteArrayOf(0xEF.toByte(), 0xBB.toByte(), 0xBF.toByte())

/**
 * CSV 내보내기 동작 — `(파일 이름, 내용)` 을 받아 문서 선택기를 연다. 결과는 토스트(§6.2a-2)다: 저장하면 정보,
 * 못 쓰면 오류. 선택기를 닫으면 아무것도 하지 않는다.
 */
@Composable
fun rememberCsvExport(): (String, () -> String) -> Unit {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var pending by remember { mutableStateOf<String?>(null) }
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/csv")) { uri ->
        val text = pending
        pending = null
        if (uri == null || text == null) return@rememberLauncherForActivityResult
        scope.launch {
            val r = withContext(Dispatchers.IO) {
                runCatching {
                    context.contentResolver.openOutputStream(uri)?.use { it.write(BOM + text.toByteArray(Charsets.UTF_8)) }
                        ?: error("저장할 곳을 열 수 없습니다")
                }
            }
            DispatchService.session?.notify(
                if (r.isSuccess) NoticeLevel.INFO else NoticeLevel.ERROR,
                if (r.isSuccess) "CSV 를 저장했습니다" else "CSV 를 저장하지 못했습니다",
                r.exceptionOrNull()?.message.orEmpty())
        }
    }
    return { name, content -> pending = content(); launcher.launch(name) }
}
