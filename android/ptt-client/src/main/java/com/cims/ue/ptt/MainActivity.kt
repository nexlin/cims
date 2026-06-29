package com.cims.ue.ptt

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/**
 * ptt-client (M2) 스캐폴드. 비-PJSIP 코어 컴포넌트(TS 규격)는 구현 완료:
 *  - floor/  : TS 24.380 RTCP-APP "MCPT" 코덱([com.cims.ue.ptt.floor.FloorCodec]) + [com.cims.ue.ptt.floor.FloorClient] 상태머신
 *  - csc/    : OAuth2 PKCE([com.cims.ue.ptt.csc.Pkce]) + [com.cims.ue.ptt.csc.CscClient](IdMS/GMS/CMS)
 *  - mcptt/  : [com.cims.ue.ptt.mcptt.McpttXml] (mcptt-info / resource-lists / affiliation / GMS 파서)
 *
 * SIP 의존 배선(affiliation PUBLISH·키업 그룹 INVITE multipart·core SipController 연동)은 M2-PJSIP 단계.
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme { Surface(modifier = Modifier.fillMaxSize()) { PttScaffold() } }
        }
    }
}

@Composable
private fun PttScaffold() {
    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text("CIMS MCPTT (M2)", style = MaterialTheme.typography.titleLarge)
        Text("3GPP TS 규격 기반 PTT 클라이언트 (CMP simplified 포맷이 아닌 TS 24.380/24.379 정합).",
            style = MaterialTheme.typography.bodySmall)
        HorizontalDivider()
        Text("구현(비-PJSIP, TS 규격):", style = MaterialTheme.typography.titleMedium)
        Text("• Floor: TS 24.380 RTCP-APP \"MCPT\" 코덱 + UDP 상태머신", style = MaterialTheme.typography.bodyMedium)
        Text("• CSC: TS 33.180 OAuth2 PKCE(S256) + GMS/CMS XCAP(OkHttp)", style = MaterialTheme.typography.bodyMedium)
        Text("• MCPTT XML: mcptt-info / resource-lists / affiliation-command + GMS 그룹문서 파서",
            style = MaterialTheme.typography.bodyMedium)
        HorizontalDivider()
        Text("다음(M2-PJSIP): affiliation PUBLISH · 키업 그룹 INVITE(multipart) · core SipController 연동 · floor↔오디오 브리지.",
            style = MaterialTheme.typography.bodySmall)
    }
}
