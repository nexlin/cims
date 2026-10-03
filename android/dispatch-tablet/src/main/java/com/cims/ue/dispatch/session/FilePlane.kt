// 관제 세션의 파일 평면 — MCData FD (docs/design/features/android_dispatch_tablet.md §6.2e, mcdata_messaging.md §4.5)
//
// 업로드(CSC 콘텐츠 서버 `POST /mcdata/fd`) → FD 알림(SIP MESSAGE — FILEURL + 이름·크기·종류) → 받는 쪽이 FILEURL 로 다운로드.
// 데스크톱 `DispatchSession` «MCData FD» 절과 `McDataMessagesViewModel.SendFileAsync`·`SendFileCore`·`DownloadFile`·`OpenFile` 에
// 대응한다. 규칙은 그쪽과 같고, 달라지는 것은 Android 접점뿐이다:
//
//  · 고르기 = 시스템 파일 고르개(content Uri). 고른 파일은 **앱 저장소에 사본을 둔다** — 원본의 접근 권한에 기대지 않는다
//    (주소록 CSV 와 같은 판단). 재전송이 그 사본을 다시 읽고, 보낸 말풍선의 [열기] 도 그 사본을 연다.
//  · 받은 파일 = 앱 전용 «받은 파일» 폴더(`files/mcdata/received`). 보관이 로그인 ID 로 격리돼 있으므로(§6.9) 다른 자리가
//    기기의 공용 폴더에서 훑어볼 수 있는 자리에 두지 않는다. 여는 것은 FileProvider + ACTION_VIEW 다.
//  · 폴더는 보관 정리를 따른다 — 어느 말풍선도 가리키지 않게 된 파일은 기동 때 지운다([sweepFiles]).
//
// 그룹 FD 는 서버가 그 그룹의 `allow_fd`·멤버십으로 게이트한다(업로드에 그룹을 싣는다). 관제석은 받은 파일을 자동으로 받지
// 않는다 — 그룹 파일이 쌓이는 자리라 관제사가 고른다.
package com.cims.ue.dispatch.session

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import androidx.core.content.FileProvider
import com.cims.ue.sdk.CimsResult
import com.cims.ue.sdk.FdFile
import com.cims.ue.sdk.FdUpload
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.util.Locale

/** FileProvider authority 의 꼬리 — 매니페스트 `${applicationId}.fileprovider` 와 같다. */
private const val FILE_AUTHORITY_SUFFIX = ".fileprovider"
private const val FILE_TAG = "DispatchFile"

/**
 * 파일 평면의 순수 규칙 — JVM 에서 시험한다(크기 낱말·상한·이름 겹침 «(n)»·받기 자격·폴더 자리).
 */
internal object FileRules {
    /** 발신 파일 상한 — 서버 `McDataFd.MaxBytes` 기본값. 넘는 파일은 **읽기 전에** 막는다(메모리에 통째로 올리는 경로다). */
    const val MAX_BYTES = 50L * 1024 * 1024
    const val DEFAULT_NAME = "file.bin"
    const val OCTET_STREAM = "application/octet-stream"
    const val UPLOADING = "올리는 중…"
    const val DOWNLOADING = "받는 중…"
    /** 파일 이름의 바이트 상한 — 파일 시스템 한도(255) 안쪽에 «(n)» 과 확장자의 자리를 남긴다. */
    private const val NAME_BYTES = 200

    fun tooLarge(size: Long): Boolean = size > MAX_BYTES
    val tooLargeText: String get() = "파일이 너무 큽니다 — 최대 ${MAX_BYTES / (1024 * 1024)} MB"
    /** 상한 토스트의 ▸상세 — «보고서.pdf · 61.3 MB». */
    fun tooLargeDetail(name: String, size: Long): String = "$name · ${oneDecimal(size / (1024.0 * 1024))} MB"

    /** 크기 한 낱말(데스크톱 `FileSizeText`) — «812 B» · «340 KB» · «1.2 MB». 모르면(0 이하) 빈 값. */
    fun sizeText(bytes: Long): String = when {
        bytes <= 0 -> ""
        bytes < 1024 -> "$bytes B"
        bytes < 1024 * 1024 -> "${oneDecimal(bytes / 1024.0)} KB"
        else -> "${oneDecimal(bytes / (1024.0 * 1024))} MB"
    }

    /** 소수 한 자리까지, 0 이면 뗀다(«1.2»·«340»). */
    private fun oneDecimal(v: Double): String =
        String.format(Locale.ROOT, "%.1f", v).removeSuffix(".0")

    /**
     * 보내기 전에 올려야 하는가 — FILEURL 이 아직 없다. 재전송의 갈림이다: 못 올리고 실패한 말풍선은 업로드부터, 올린 뒤
     * 알림이 실패한 말풍선은 **알림만** 다시 보낸다(같은 파일을 두 번 올리지 않는다 — 데스크톱 `SendFileCore`).
     */
    fun needsUpload(m: Message): Boolean = m.fileUrl.isEmpty()

    /** [받기] 가 서는가(데스크톱 `CanDownload`) — 받은 파일 · 기기에 아직 없음 · 받는 중 아님 · 받을 주소 있음. */
    fun canDownload(m: Message, hasLocalFile: Boolean): Boolean =
        m.isAttachment && !m.outgoing && !hasLocalFile && !m.isTransferring && m.fileUrl.isNotEmpty()

    /**
     * 저장할 수 있는 이름 — 경로 문자·제어 문자는 `_` 로, 앞뒤 공백과 머리의 점은 뗀다(숨김 파일·상위 폴더 `..` 가 되지 않게).
     * 비면 `file.bin`. 너무 긴 이름은 확장자를 남기고 줄인다. 보낸 쪽이 준 이름이라 그대로 믿지 않는다.
     */
    fun safeName(name: String): String {
        val n = name.map { c -> if (c in "/\\:*?\"<>|" || c.code < 0x20) '_' else c }.joinToString("").trim().trimStart('.')
        if (n.isEmpty()) return DEFAULT_NAME
        val (whole, ext) = split(n)
        var stem = whole
        while (stem.length > 1 && (stem + ext).toByteArray(Charsets.UTF_8).size > NAME_BYTES) stem = stem.dropLast(1)
        return stem + ext
    }

    /** 겹치지 않는 이름(데스크톱 `AppPaths.UniqueFile`) — 있으면 «이름 (1).확장자», «이름 (2).확장자» … */
    fun uniqueName(name: String, exists: (String) -> Boolean): String {
        val safe = safeName(name)
        if (!exists(safe)) return safe
        val (stem, ext) = split(safe)
        var i = 1
        while (exists("$stem ($i)$ext")) i++
        return "$stem ($i)$ext"
    }

    /** (줄기, 확장자 — 점 포함). 점으로 시작하거나 점이 없으면 확장자는 없다. */
    private fun split(name: String): Pair<String, String> {
        val dot = name.lastIndexOf('.')
        return if (dot > 0) name.substring(0, dot) to name.substring(dot) else name to ""
    }

    /** 파일 평면의 폴더 — FileProvider 가 내주는 자리(`res/xml/file_paths.xml` 의 `mcdata/`)와 같다. */
    fun root(filesDir: File): File = File(filesDir, "mcdata")
    /** 받은 파일. */
    fun receivedDir(filesDir: File): File = File(root(filesDir), "received")
    /** 보낸 파일의 사본. */
    fun sentDir(filesDir: File): File = File(root(filesDir), "sent")
}

/** 고른 파일이 상한을 넘었다 — 크기를 미리 알려 주지 않는 제공자는 읽다가 안다(그때는 전체 크기를 모른다). */
private class FileTooLarge : IOException("file too large")

/**
 * 어느 말풍선도 가리키지 않는 파일을 지운다 — 보관 기간이 지나 말풍선이 지워지면 그 파일(보낸 사본·받은 파일)도 기기에 남길
 * 까닭이 없다. 방금 생긴 파일([minAgeMs] 안)은 건드리지 않는다 — 사본을 쓰고 경로를 보관에 적기까지의 틈이 있다.
 *
 * @param keep 보관의 말풍선이 가리키는 경로 전부(주인 무관 — `MessageStore.localPaths`)
 * @return 지운 파일 수
 */
internal fun sweepFiles(root: File, keep: Set<String>, nowMs: Long = System.currentTimeMillis(),
                        minAgeMs: Long = 10 * 60_000L): Int {
    if (!root.isDirectory) return 0
    var n = 0
    root.walkTopDown().filter { it.isFile }.forEach { f ->
        if (f.absolutePath !in keep && nowMs - f.lastModified() >= minAgeMs && f.delete()) n++
    }
    return n
}

/** 지금의 말풍선 — 화면이 넘긴 사본은 낡았을 수 있다(진행 문구·경로가 그 사이 바뀐다). */
private fun DispatchSession.fileMessage(id: String): Message? =
    messages.value.values.firstNotNullOfOrNull { list -> list.firstOrNull { it.id == id } }

// ── 발신 ─────────────────────────────────────────────────────────────────────

/**
 * 고른 파일 하나를 스레드로 보낸다 — [key] 가 편성 그룹이면 그룹 FD, 아니면 1:1 FD(`sendSdsTo` 와 같은 판정·같은 스레드 키).
 *
 * 빈 파일·상한 초과는 **읽기 전에** 막는다. 그다음 사본을 앱 저장소에 두고, 말풍선을 먼저 세운 뒤(«올리는 중…») 올린다 —
 * 올리기를 기다렸다 세우면 큰 파일에서 «눌렀는데 아무 일도 없는» 구간이 길다.
 */
suspend fun DispatchSession.sendFile(context: Context, key: String, uri: Uri) {
    // 여러 파일을 고르면 세션 수명에서 차례로 돈다 — 그 사이 로그아웃했으면 남은 파일을 보내지 않는다(비운 대화에 말풍선을
    //   세우면 다음 로그인의 보관 적재가 그것을 제 것 위에 얹는다)
    val gen = loginGeneration.value
    val meta = withContext(Dispatchers.IO) { runCatching { pickedMeta(context, uri) } }.getOrElse {
        notify(NoticeLevel.ERROR, "파일을 읽을 수 없습니다", it.message.orEmpty()); return
    }
    val name = FileRules.safeName(meta.first)
    if (meta.second == 0L) { notify(NoticeLevel.INFO, "빈 파일은 보낼 수 없습니다", name); return }
    if (FileRules.tooLarge(meta.second)) {
        notify(NoticeLevel.ERROR, FileRules.tooLargeText, FileRules.tooLargeDetail(name, meta.second)); return
    }
    val copy = withContext(Dispatchers.IO) { runCatching { copyPicked(context, uri, name) } }.getOrElse {
        if (it is FileTooLarge) notify(NoticeLevel.ERROR, FileRules.tooLargeText, name)
        else notify(NoticeLevel.ERROR, "파일을 읽을 수 없습니다", it.message.orEmpty())
        return
    }
    if (gen != loginGeneration.value) { withContext(Dispatchers.IO) { copy.delete() }; return }
    if (copy.length() == 0L) {                       // 크기를 알려 주지 않던 제공자의 빈 파일
        withContext(Dispatchers.IO) { copy.delete() }
        notify(NoticeLevel.INFO, "빈 파일은 보낼 수 없습니다", name); return
    }
    val m = addOutgoingFile(key, name, copy.length(), meta.third, copy.absolutePath, FileRules.UPLOADING)
    sendFileCore(m.id)
}

/** 실패한 파일 말풍선 다시 보내기 — 같은 말풍선이 갱신된다. 이미 올린 파일(FILEURL 있음)은 알림만 다시 보낸다. */
suspend fun DispatchSession.resendFile(m: Message) {
    if (m.kind != MessageKind.SDS || !m.isAttachment) return
    if (!beginResend(m)) return                        // 이미 다시 보내는 중이거나 실패가 아니다
    sendFileCore(m.id)
}

/**
 * 업로드(FILEURL 이 아직 없으면) → FD 알림 발신. 재전송도 여기로 온다(데스크톱 `SendFileCore`).
 *
 * 최종 응답은 글과 같이 token 으로 맞춘다(`applyRequestResult`) — 알림을 보낸 뒤 «보내는 중» 으로 남고, 2xx 가 오면 «보냄» 이다.
 */
private suspend fun DispatchSession.sendFileCore(id: String) {
    var m = patchMessage(id, persist = false) { it.copy(state = SendState.PENDING) } ?: return
    val group = isPttGroup(m.groupId)
    var hash = ""                                   // 올린 바이트의 SHA-1 — 이번에 올렸을 때만 안다(재전송은 싣지 않는다, RFC 5547 hash 는 선택)
    if (FileRules.needsUpload(m)) {
        if (!m.hasLocalFile) return failFile(id, "원본 파일이 없어 다시 보낼 수 없습니다", m.localPath)
        patchMessage(id, persist = false) { it.copy(transferNote = FileRules.UPLOADING) }
        val path = m.localPath
        val data = withContext(Dispatchers.IO) { runCatching { File(path).readBytes() } }.getOrElse {
            return failFile(id, "파일을 읽을 수 없습니다", it.message.orEmpty())
        }
        val up = uploadFile(data, m.fileName, m.fileType, if (group) m.groupId else "")
        patchMessage(id, persist = false) { it.copy(transferNote = "") } ?: return
        val url = up.value?.url?.takeIf { up.ok } ?: return failFile(id, null)   // 사유는 [uploadFile] 이 토스트로 냈다
        hash = up.value?.hash.orEmpty()
        m = patchMessage(id) { it.copy(fileUrl = url) } ?: return
    }
    val ptt = pttAccount ?: run {
        failFile(id, null)
        report(TextArea.SDS, CimsResult.fail<Unit>(-1, "PTT 계정 없음")); return
    }
    val file = FdFile(url = m.fileUrl, name = m.fileName, type = m.fileType, size = m.fileSize, hash = hash)   // Metadata file-selector(§15.2.17)
    val target = m.groupId
    val (r, early) = sendTracked({ it.token }) {
        if (group) ptt.sendGroupFd(target, file) else ptt.sendFd(userPart(target).ifEmpty { target }, file)
    }
    // 명령이 돌아온 그 자리에서 token 을 건다(대기 없이) — 최종 응답이 먼저 와 있었으면 그 상태로 선다.
    val sent = r.value?.takeIf { r.ok }
    val st = when {
        sent == null -> SendState.FAILED
        early != null -> if (early.code in 200..299) SendState.SENT else SendState.FAILED
        else -> SendState.PENDING
    }
    patchMessage(id) { it.copy(msgId = sent?.msgId ?: it.msgId, token = sent?.token ?: it.token, state = st) }
    if (sent == null) report(TextArea.SDS, CimsResult.fail<Unit>(r.code, r.reason))
    else if (st == SendState.FAILED && early != null)
        notify(NoticeLevel.ERROR, ResponseText.sip(TextArea.SDS, early.code, early.reason), "${early.code} ${early.reason}".trim())
}

/** 파일 말풍선을 실패로 닫는다 — [title] 이 있으면 토스트(없으면 사유는 이미 나갔다). */
private fun DispatchSession.failFile(id: String, title: String?, detail: String = "") {
    patchMessage(id) { it.copy(state = SendState.FAILED, transferNote = "") }
    if (title != null) notify(NoticeLevel.ERROR, title, detail)
}

/**
 * 파일 업로드 — [groupId] 가 있으면 그룹 FD(서버 `allow_fd`·멤버십 게이트), 비면 1:1. 전용 CSC 핸들로 보낸다(큰 파일이
 * 다른 조회를 막지 않게 — `fdCscOrNull`). 401 은 토큰을 강제로 갱신해 한 번 더 보낸다. 실패 사유는 여기서 토스트로 낸다.
 */
internal suspend fun DispatchSession.uploadFile(data: ByteArray, name: String, mime: String, groupId: String): CimsResult<FdUpload> {
    val csc = fdCscOrNull()
    val tk = if (csc == null) null else accessToken()
    if (csc == null || tk == null) return report(TextArea.FILE, CimsResult.fail(-1, "로그인 전"))
    // 규격형 업로드(TS 24.282 §10.2.2.1) — 발신 MCData ID = PTT 신원(단일 MC 서비스 ID)
    var r = csc.uploadFd(tk, data, name, mime, groupId, myPttId)
    if (!r.ok && r.code == 401) renewAccessToken()?.takeIf { it != tk }?.let { fresh ->
        r = csc.uploadFd(fresh, data, name, mime, groupId, myPttId)
    }
    if (r.ok) android.util.Log.i(FILE_TAG, "fd upload $name ${data.size}B group=${groupId.ifEmpty { "-" }} → ${r.value?.id}")
    else {
        android.util.Log.w(FILE_TAG, "fd upload $name: ${r.code} ${r.reason}")
        notify(NoticeLevel.ERROR, ResponseText.of(TextArea.FILE, r.code, r.reason), "${r.code} ${r.reason}".trim())
    }
    return r
}

// ── 수신 ─────────────────────────────────────────────────────────────────────

/**
 * 받은 파일 받기([받기]) — «받는 중…» → 받은 파일 폴더에 저장(같은 이름이면 «(n)») → 경로를 말풍선·보관에 적고 → 연다.
 * 받을 수 있는지는 **지금의 말풍선**으로 본다 — 받는 동안 한 번 더 눌러도 두 번 받지 않는다.
 */
suspend fun DispatchSession.downloadFile(context: Context, m: Message) {
    val cur = fileMessage(m.id) ?: return
    if (!cur.canDownload) return
    patchMessage(cur.id, persist = false) { it.copy(transferNote = FileRules.DOWNLOADING) }
    val r = fetchFile(context, cur.fileUrl, cur.fileName)
    patchMessage(cur.id, persist = false) { it.copy(transferNote = "") } ?: return
    val path = r.value?.takeIf { r.ok } ?: return
    patchMessage(cur.id) { it.copy(localPath = path) } ?: return
    openFile(context, cur)
}

/**
 * FILEURL 다운로드 → 받은 파일 폴더. 코어가 FILEURL 의 **경로만** 취해 자기 CSC 로 보낸다(Bearer 를 다른 호스트로 보내지
 * 않는다 — `CscClient.downloadFd`). 401 은 강제 갱신 후 한 번 재시도. 반환 = 저장한 경로.
 */
private suspend fun DispatchSession.fetchFile(context: Context, url: String, name: String): CimsResult<String> {
    val csc = fdCscOrNull()
    val tk = if (csc == null) null else accessToken()
    if (csc == null || tk == null) return report(TextArea.FILE, CimsResult.fail(-1, "로그인 전"))
    var r = csc.downloadFd(tk, url)
    if (!r.ok && r.code == 401) renewAccessToken()?.takeIf { it != tk }?.let { fresh -> r = csc.downloadFd(fresh, url) }
    if (!r.ok) {
        val body = r.value?.body?.takeIf { it.isNotEmpty() }?.toString(Charsets.UTF_8) ?: r.reason
        android.util.Log.w(FILE_TAG, "fd download $url: ${r.code} $body")
        notify(NoticeLevel.ERROR, ResponseText.of(TextArea.FILE, r.code, body), "${r.code} ${r.reason}".trim())
        return CimsResult.fail(r.code, body)
    }
    val bytes = r.value?.body ?: ByteArray(0)
    val filesDir = context.filesDir
    return withContext(Dispatchers.IO) {
        runCatching {
            val dir = FileRules.receivedDir(filesDir).apply { mkdirs() }
            File(dir, FileRules.uniqueName(name.ifEmpty { FileRules.DEFAULT_NAME }) { File(dir, it).exists() })
                .apply { writeBytes(bytes) }.absolutePath
        }
    }.fold(
        onSuccess = { android.util.Log.i(FILE_TAG, "fd download $url → $it (${bytes.size}B)"); CimsResult.ok(it) },
        onFailure = {
            android.util.Log.w(FILE_TAG, "fd download $url: cannot write — ${it.message}")
            notify(NoticeLevel.ERROR, "받은 파일을 저장할 수 없습니다", it.message.orEmpty())
            CimsResult.fail(-3, it.message.orEmpty())
        })
}

/**
 * 연다 — 연결된 앱으로(FileProvider + ACTION_VIEW). 받지 않은 수신 파일이면 받기부터 한다(데스크톱 `OpenFile`).
 * 보낸 말풍선은 앱이 둔 사본을 연다.
 */
suspend fun DispatchSession.openFile(context: Context, m: Message) {
    val cur = fileMessage(m.id) ?: return
    if (!cur.hasLocalFile) { if (cur.canDownload) downloadFile(context, cur); return }
    runCatching {
        val file = File(cur.localPath)
        val uri = FileProvider.getUriForFile(context, context.packageName + FILE_AUTHORITY_SUFFIX, file)
        // 종류는 확장자가 먼저다 — 보낸 쪽이 적은 종류는 `application/octet-stream` 이기 쉬워 그대로 쓰면 열 앱을 못 찾는다.
        val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(file.extension.lowercase(Locale.ROOT))
            ?: cur.fileType.ifEmpty { FileRules.OCTET_STREAM }
        context.startActivity(Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, mime)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        })
    }.onFailure {
        notify(NoticeLevel.ERROR, "파일을 열 수 없습니다",
            if (it is ActivityNotFoundException) "이 종류의 파일을 열 앱이 없습니다 — ${cur.fileName}" else it.message.orEmpty())
    }
}

// ── Android 접점 — 고른 문서 읽기 ─────────────────────────────────────────────

/** 고른 문서의 (이름, 크기, 종류). 크기를 주지 않는 제공자는 -1 — 그때는 읽으면서 센다. */
private fun pickedMeta(context: Context, uri: Uri): Triple<String, Long, String> {
    var name = uri.lastPathSegment?.substringAfterLast('/').orEmpty()
    var size = -1L
    context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
        if (c.moveToFirst()) {
            c.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { it >= 0 && !c.isNull(it) }?.let { name = c.getString(it) }
            c.getColumnIndex(OpenableColumns.SIZE).takeIf { it >= 0 && !c.isNull(it) }?.let { size = c.getLong(it) }
        }
    }
    return Triple(name, size, context.contentResolver.getType(uri) ?: FileRules.OCTET_STREAM)
}

/** 고른 문서 → 보낸 파일 폴더의 사본. 상한을 넘으면 거기서 멈추고 사본을 지운다([FileTooLarge]). */
private fun copyPicked(context: Context, uri: Uri, name: String): File {
    val dir = FileRules.sentDir(context.filesDir).apply { mkdirs() }
    val out = File(dir, FileRules.uniqueName(name) { File(dir, it).exists() })
    try {
        val ins = context.contentResolver.openInputStream(uri) ?: throw IOException("열 수 없는 파일입니다")
        ins.use { src ->
            out.outputStream().use { dst ->
                val buf = ByteArray(64 * 1024)
                var total = 0L
                while (true) {
                    val n = src.read(buf)
                    if (n < 0) break
                    total += n
                    if (FileRules.tooLarge(total)) throw FileTooLarge()
                    dst.write(buf, 0, n)
                }
            }
        }
    } catch (t: Throwable) {
        out.delete()
        throw t
    }
    return out
}
