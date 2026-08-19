package com.cims.ue.cims

import android.accounts.Account
import android.accounts.AccountAuthenticatorResponse
import android.accounts.AccountManager
import android.content.ComponentName
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.core.CimsSuite
import com.cims.ue.core.account.CimsAccounts
import com.cims.ue.core.provision.CscEndpoint
import com.cims.ue.core.provision.ProvisioningClient

/**
 * CIMS 프로비저닝 로그인 — 1회 로그인으로 공유 계정(AccountManager)을 생성한다.
 * 로그인은 IdMS PKCE(두 scope grant) → refresh_token 을 계정에 보관 → 이후 모든 앱이
 * authTokenType 별(provisioning / mcptt) 로 토큰을 받아 쓴다.
 */
class LoginActivity : ComponentActivity() {

    private val response: AccountAuthenticatorResponse? by lazy {
        @Suppress("DEPRECATION")
        if (Build.VERSION.SDK_INT >= 33)
            intent.getParcelableExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE, AccountAuthenticatorResponse::class.java)
        else
            intent.getParcelableExtra(AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE)
    }

    /** 현재 로그인된 계정명(없으면 null) — 로그인/로그아웃에 따라 화면 상태 전환. */
    private val loggedInUser = mutableStateOf<String?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val am = AccountManager.get(this)
        val existing = CimsAccounts.get(am)
        val prefillUser = intent.getStringExtra(AccountManager.KEY_ACCOUNT_NAME) ?: existing?.name ?: ""
        loggedInUser.value = existing?.name

        setContent {
            // 시안 다크 고정 — PTT/Phone 과 같은 다크·민트 톤.
            MaterialTheme(colorScheme = CimsDark) {
                Box(Modifier.fillMaxSize().background(Cl.Bg)) {
                    LoginScreen(
                        initialUser = prefillUser,
                        loggedInUser = loggedInUser.value,
                        onSubmit = ::doLogin,
                        onLogout = ::doLogout,
                    )
                }
            }
        }
    }

    private fun doLogin(
        host: String, port: Int, user: String, password: String,
        onResult: (ok: Boolean, msg: String) -> Unit,
    ) {
        Thread {
            try {
                val ep = CscEndpoint(host = host.trim(), port = port)
                val ts = ProvisioningClient(ep).login(user.trim(), password)
                val refresh = ts.refreshToken
                    ?: throw IllegalStateException("서버가 refresh_token 을 발급하지 않음")

                val am = AccountManager.get(this)
                val account = Account(user.trim(), CimsAccounts.ACCOUNT_TYPE)
                val existing = am.getAccountsByType(CimsAccounts.ACCOUNT_TYPE).firstOrNull { it.name == account.name }
                if (existing == null) {
                    am.addAccountExplicitly(account, refresh, null)  // password 슬롯 = refresh_token
                } else {
                    am.setPassword(account, refresh)
                }
                am.setUserData(account, CimsAccounts.KEY_CSC_HOST, host.trim())
                am.setUserData(account, CimsAccounts.KEY_CSC_PORT, port.toString())
                am.setUserData(account, CimsAccounts.KEY_LOGIN_PW, password)  // SIP Digest 재사용용(동일서명 보호)
                am.setAuthToken(account, CimsAccounts.TOKEN_PROVISIONING, ts.accessToken)

                response?.onResult(Bundle().apply {
                    putString(AccountManager.KEY_ACCOUNT_NAME, account.name)
                    putString(AccountManager.KEY_ACCOUNT_TYPE, account.type)
                })
                runOnUiThread {
                    loggedInUser.value = account.name
                    startCompanionServices()
                    onResult(true, "로그인 성공 — 계정이 등록되었습니다")
                    finish()
                }
            } catch (e: Exception) {
                runOnUiThread { onResult(false, "로그인 실패: ${e.message}") }
            }
        }.start()
    }

    /**
     * 로그아웃 — 공유 계정 제거(캐시 토큰도 함께 소멸) 후 스위트 로그아웃 브로드캐스트.
     * Phone/PTT 는 수신 시 등록 해제 + 프로비저닝 설정 제거 + 등록유지 FGS 종료하고,
     * 이후 실행 시 이 로그인 화면으로 유도된다. 정지 상태 앱에도 배달(INCLUDE_STOPPED).
     */
    private fun doLogout(onDone: (String) -> Unit) {
        Thread {
            val am = AccountManager.get(this)
            am.getAccountsByType(CimsAccounts.ACCOUNT_TYPE).forEach { acct ->
                runCatching { am.removeAccountExplicitly(acct) }
            }
            listOf(CimsSuite.VOLTE_PACKAGE, CimsSuite.PTT_PACKAGE).forEach { pkg ->
                runCatching {
                    sendBroadcast(
                        Intent(CimsSuite.ACTION_LOGOUT).setPackage(pkg)
                            .addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES),
                        CimsSuite.PERMISSION,
                    )
                }
            }
            runOnUiThread {
                loggedInUser.value = null
                onDone("로그아웃되었습니다 — Phone/PTT 등록이 해제됩니다")
            }
        }.start()
    }

    /**
     * 로그인 직후 CIMS-Phone/McPtt 등록유지 서비스를 즉시 시작 — 앱을 한 번도 열지 않아도
     * 백그라운드에서 SIP 등록이 유지돼 착신 가능(기본 전화앱처럼). 포그라운드 Activity 발신이라
     * FGS 시작이 허용되며, 미설치 앱은 조용히 건너뛴다. 부팅 이후는 각 앱 BootReceiver 가 담당.
     */
    private fun startCompanionServices() {
        listOf(
            ComponentName("com.cims.ue.volte", "com.cims.ue.volte.SipService"),
            ComponentName("com.cims.ue.ptt", "com.cims.ue.ptt.PttService"),
        ).forEach { cn ->
            runCatching {
                val i = Intent().setComponent(cn).putExtra("autostart", true)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(i)
                else startService(i)
            }
        }
    }
}

/** 디자인 토큰 — 시안(assets/pages/로그인화면, 다크 배경 + 민트 액센트). PTT 앱 Ct 와 동일 값. */
private object Cl {
    val Bg = Color(0xFF0D1211)
    val Surface = Color(0xFF151C1A)
    val SurfaceHi = Color(0xFF1B2422)
    val Border = Color(0xFF243230)
    val Mint = Color(0xFF5EE0C0)
    val OnMint = Color(0xFF0C1512)
    val Text = Color(0xFFECF3F1)
    val TextDim = Color(0xFF8FA39E)
    val TextFaint = Color(0xFF5E6E6A)
    val Red = Color(0xFFEF5350)
    val RedDim = Color(0xFF3A1B1B)
    val GrayDim = Color(0xFF232B29)
}

private val CimsDark = darkColorScheme(
    primary = Cl.Mint, onPrimary = Cl.OnMint,
    background = Cl.Bg, onBackground = Cl.Text,
    surface = Cl.Surface, onSurface = Cl.Text,
    surfaceVariant = Cl.SurfaceHi, onSurfaceVariant = Cl.TextDim,
    outline = Cl.Border, error = Cl.Red,
)

@Composable
private fun LoginScreen(
    initialUser: String,
    loggedInUser: String?,
    onSubmit: (host: String, port: Int, user: String, password: String, cb: (Boolean, String) -> Unit) -> Unit,
    onLogout: (cb: (String) -> Unit) -> Unit,
) {
    var host by remember { mutableStateOf("121.161.164.45") }
    var port by remember { mutableStateOf("4430") }
    var user by remember { mutableStateOf(initialUser) }
    var password by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("") }
    var failed by remember { mutableStateOf(false) }
    var serverOpen by remember { mutableStateOf(false) }
    var confirmLogout by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = 28.dp)
            .verticalScroll(rememberScrollState()).imePadding(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        // 로고 + 타이틀 (시안: 상단 중앙 로고, 아래 서비스명)
        Image(
            painterResource(R.drawable.yrt_logo), contentDescription = null,
            modifier = Modifier.width(180.dp),
        )
        Spacer(Modifier.height(10.dp))
        Text("CIMS 통합 로그인", color = Cl.Mint, fontSize = 16.sp, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(4.dp))
        Text("1회 로그인으로 CIMS-Phone · McPTT 가 함께 사용합니다",
            color = Cl.TextDim, fontSize = 12.sp)
        if (loggedInUser != null) {
            Spacer(Modifier.height(8.dp))
            Text("$loggedInUser 로그인됨 — 재로그인하면 갱신됩니다",
                color = Cl.Mint, fontSize = 12.sp,
                modifier = Modifier.clip(RoundedCornerShape(50)).background(Cl.Mint.copy(alpha = 0.12f))
                    .padding(horizontal = 10.dp, vertical = 3.dp))
        }
        Spacer(Modifier.height(26.dp))

        FieldLabel("아이디")
        DarkField(user, { user = it }, placeholder = "로그인 ID (예: test001)")
        Spacer(Modifier.height(12.dp))
        FieldLabel("비밀번호")
        DarkField(password, { password = it }, placeholder = "비밀번호",
            password = true, keyboardType = KeyboardType.Password)
        Spacer(Modifier.height(10.dp))

        // 서버 설정 — 평소 접힘(시안에 없음), 값 변경이 필요할 때만 펼침.
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp))
                .clickable { serverOpen = !serverOpen }.padding(vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(if (serverOpen) "서버 설정 ▾" else "서버 설정 ▸", color = Cl.TextFaint, fontSize = 12.sp)
            Spacer(Modifier.width(8.dp))
            Text("$host:$port", color = Cl.TextFaint, fontSize = 12.sp)
        }
        if (serverOpen) {
            Spacer(Modifier.height(4.dp))
            FieldLabel("CSC 주소")
            DarkField(host, { host = it }, placeholder = "서버 주소")
            Spacer(Modifier.height(12.dp))
            FieldLabel("CSC 포트")
            DarkField(port, { port = it.filter { c -> c.isDigit() } }, placeholder = "4430",
                keyboardType = KeyboardType.Number)
        }
        Spacer(Modifier.height(22.dp))

        // 로그인 버튼 — 시안의 큰 민트 버튼.
        val enabled = !busy && host.isNotBlank() && user.isNotBlank() && password.isNotBlank()
        Box(
            Modifier.fillMaxWidth().height(52.dp).clip(RoundedCornerShape(14.dp))
                .background(if (enabled) Cl.Mint else Cl.GrayDim)
                .clickable(enabled = enabled) {
                    busy = true; failed = false; status = "로그인 중…"
                    onSubmit(host, port.toIntOrNull() ?: 4430, user, password) { ok, msg ->
                        busy = false; failed = !ok; status = msg
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            if (busy) CircularProgressIndicator(Modifier.size(22.dp), color = Cl.OnMint, strokeWidth = 2.5.dp)
            else Text("로그인", color = if (enabled) Cl.OnMint else Cl.TextFaint,
                fontSize = 16.sp, fontWeight = FontWeight.Bold)
        }

        // 로그아웃 — 로그인 상태에서만. 확인 후 계정 제거 + Phone/PTT 종료 통지.
        if (loggedInUser != null) {
            Spacer(Modifier.height(12.dp))
            Box(
                Modifier.fillMaxWidth().height(48.dp).clip(RoundedCornerShape(14.dp))
                    .border(1.dp, Cl.Red.copy(alpha = 0.6f), RoundedCornerShape(14.dp))
                    .clickable(enabled = !busy) { confirmLogout = true },
                contentAlignment = Alignment.Center,
            ) {
                Text("로그아웃", color = Cl.Red, fontSize = 15.sp, fontWeight = FontWeight.Bold)
            }
        }
        if (confirmLogout) {
            AlertDialog(
                onDismissRequest = { confirmLogout = false },
                containerColor = Cl.Surface,
                title = { Text("로그아웃", color = Cl.Text) },
                text = { Text("로그아웃하면 CIMS-Phone / McPTT 의 등록이 해제되고 앱이 종료됩니다.",
                    color = Cl.TextDim) },
                confirmButton = {
                    TextButton(onClick = {
                        confirmLogout = false
                        busy = true; failed = false; status = "로그아웃 중…"
                        onLogout { msg -> busy = false; status = msg }
                    }) { Text("로그아웃", color = Cl.Red) }
                },
                dismissButton = {
                    TextButton(onClick = { confirmLogout = false }) { Text("취소", color = Cl.TextDim) }
                },
            )
        }

        // 상태/오류 — 실패 시 시안의 붉은 외곽선 박스.
        if (status.isNotBlank() && !busy) {
            Spacer(Modifier.height(14.dp))
            if (failed) {
                Text(status, color = Cl.Red, fontSize = 13.sp,
                    modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp))
                        .background(Cl.RedDim.copy(alpha = 0.5f))
                        .border(1.dp, Cl.Red.copy(alpha = 0.6f), RoundedCornerShape(10.dp))
                        .padding(horizontal = 12.dp, vertical = 10.dp))
            } else {
                Text(status, color = Cl.Mint, fontSize = 13.sp)
            }
        }
    }
}

@Composable
private fun FieldLabel(text: String) {
    Text(text, color = Cl.TextDim, fontSize = 12.sp, fontWeight = FontWeight.SemiBold,
        modifier = Modifier.fillMaxWidth().padding(bottom = 6.dp))
}

/** 다크 라운드 입력 필드 — 시안: 어두운 면 + 미세 테두리, 포커스 시 민트 외곽선. */
@Composable
private fun DarkField(
    value: String,
    onChange: (String) -> Unit,
    placeholder: String,
    password: Boolean = false,
    keyboardType: KeyboardType = KeyboardType.Text,
) {
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    Box(
        Modifier.fillMaxWidth().height(50.dp).clip(RoundedCornerShape(12.dp))
            .background(Cl.SurfaceHi)
            .border(1.dp, if (focused) Cl.Mint else Cl.Border, RoundedCornerShape(12.dp))
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        BasicTextField(
            value = value, onValueChange = onChange, singleLine = true,
            interactionSource = interaction,
            textStyle = TextStyle(color = Cl.Text, fontSize = 15.sp),
            cursorBrush = SolidColor(Cl.Mint),
            visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
            keyboardOptions = KeyboardOptions(keyboardType = keyboardType),
            modifier = Modifier.fillMaxWidth(),
            decorationBox = { inner ->
                if (value.isEmpty()) Text(placeholder, color = Cl.TextFaint, fontSize = 15.sp)
                inner()
            },
        )
    }
}
