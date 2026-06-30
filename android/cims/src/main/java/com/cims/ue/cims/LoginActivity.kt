package com.cims.ue.cims

import android.accounts.Account
import android.accounts.AccountAuthenticatorResponse
import android.accounts.AccountManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val am = AccountManager.get(this)
        val existing = CimsAccounts.get(am)
        val prefillUser = intent.getStringExtra(AccountManager.KEY_ACCOUNT_NAME) ?: existing?.name ?: ""

        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    LoginScreen(
                        initialUser = prefillUser,
                        alreadyLoggedIn = existing != null,
                        onSubmit = ::doLogin,
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
                val ts = ProvisioningClient(ep, allowInsecureTls = true).login(user.trim(), password)
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
                    onResult(true, "로그인 성공 — 계정이 등록되었습니다")
                    finish()
                }
            } catch (e: Exception) {
                runOnUiThread { onResult(false, "로그인 실패: ${e.message}") }
            }
        }.start()
    }
}

@androidx.compose.runtime.Composable
private fun LoginScreen(
    initialUser: String,
    alreadyLoggedIn: Boolean,
    onSubmit: (host: String, port: Int, user: String, password: String, cb: (Boolean, String) -> Unit) -> Unit,
) {
    var host by remember { mutableStateOf("121.161.164.45") }
    var port by remember { mutableStateOf("4430") }
    var user by remember { mutableStateOf(initialUser) }
    var password by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf(if (alreadyLoggedIn) "이미 로그인됨 — 재로그인하면 갱신됩니다" else "") }

    Column(
        modifier = Modifier.fillMaxSize().padding(24.dp).verticalScroll(rememberScrollState()).imePadding(),
        verticalArrangement = Arrangement.Center,
    ) {
        Text("CIMS 로그인", style = MaterialTheme.typography.headlineSmall)
        Spacer(Modifier.height(4.dp))
        Text("1회 로그인으로 CIMS-Phone / CIMS-McPtt 가 함께 사용합니다.",
            style = MaterialTheme.typography.bodySmall)
        Spacer(Modifier.height(20.dp))

        OutlinedTextField(host, { host = it }, label = { Text("CSC 주소") },
            singleLine = true, modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(port, { port = it.filter { c -> c.isDigit() } }, label = { Text("CSC 포트") },
            singleLine = true, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(user, { user = it }, label = { Text("아이디 (tel:+82...)") },
            singleLine = true, modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(password, { password = it }, label = { Text("비밀번호") },
            singleLine = true, visualTransformation = PasswordVisualTransformation(),
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(20.dp))

        Button(
            onClick = {
                busy = true; status = "로그인 중..."
                onSubmit(host, port.toIntOrNull() ?: 4430, user, password) { ok, msg ->
                    busy = false; status = msg
                }
            },
            enabled = !busy && host.isNotBlank() && user.isNotBlank() && password.isNotBlank(),
            modifier = Modifier.fillMaxWidth(),
        ) { Text(if (busy) "처리 중" else "로그인") }

        if (busy) {
            Spacer(Modifier.height(12.dp))
            CircularProgressIndicator()
        }
        if (status.isNotBlank()) {
            Spacer(Modifier.height(12.dp))
            Text(status, style = MaterialTheme.typography.bodyMedium)
        }
    }
}
