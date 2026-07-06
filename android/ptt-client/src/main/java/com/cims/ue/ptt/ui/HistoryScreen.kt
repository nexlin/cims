package com.cims.ue.ptt.ui

import android.text.format.DateFormat
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.cims.ue.ptt.PttEventKind
import com.cims.ue.ptt.PttService
import com.cims.ue.ptt.R
import com.cims.ue.ptt.history.HistEntry
import java.util.Date

private fun fmtTime(t: Long): String = DateFormat.format("HH:mm:ss", Date(t)).toString()
private fun fmtDay(t: Long): String = DateFormat.format("M월 d일 (E)", Date(t)).toString()
private fun fmtDur(ms: Long): String {
    val s = ms / 1000
    return if (s >= 60) "%d분 %d초".format(s / 60, s % 60) else "%d초".format(s.coerceAtLeast(1))
}

/** 통화이력 탭 — 발언(송·수신)/참여/긴급 이벤트 목록(일자 구분). */
@Composable
fun HistoryScreen(st: PttUiState, svc: PttService?) {
    val tick = svc?.history?.changes?.collectAsState()?.value ?: 0   // 변경 틱 구독 → 재조회
    val entries = remember(tick, svc) { svc?.history?.list() ?: emptyList() }

    Column(Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 10.dp)) {
        ScreenHeader(
            label = null, title = "통화이력",
            trailing = {
                if (entries.isNotEmpty()) {
                    Text("비우기", color = Ct.TextFaint, fontSize = 12.sp,
                        modifier = Modifier.clickable { svc?.history?.clear() })
                }
            },
        )
        Spacer(Modifier.height(10.dp))

        if (entries.isEmpty()) {
            Box(Modifier.fillMaxWidth().padding(vertical = 60.dp), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Icon(painterResource(R.drawable.ic_history), contentDescription = null,
                        tint = Ct.TextFaint, modifier = Modifier.size(36.dp))
                    Text("통화이력이 없습니다", color = Ct.TextFaint, fontSize = 13.sp)
                }
            }
            return@Column
        }

        LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            var lastDay = ""
            entries.forEachIndexed { i, e ->
                val day = fmtDay(e.time)
                if (day != lastDay) {
                    lastDay = day
                    items(listOf("day-$day-$i")) { SectionLabel(day) }
                }
                items(listOf("h-$i")) { HistoryRow(st, e) }
            }
        }
    }
}

@Composable
private fun HistoryRow(st: PttUiState, e: HistEntry) {
    val kind = runCatching { PttEventKind.valueOf(e.kind) }.getOrNull()
    val (icon, color, title) = when (kind) {
        PttEventKind.TALK_ME -> Triple(R.drawable.ic_ptt, Ct.Mint, "내 발언")
        PttEventKind.TALK_OTHER -> Triple(R.drawable.ic_voice, Ct.Amber, "${e.peer ?: "?"} 발언")
        PttEventKind.JOIN -> Triple(R.drawable.ic_connected, Ct.Gray, "채널 참여")
        PttEventKind.LEAVE -> Triple(R.drawable.ic_close, Ct.Gray, "채널 종료")
        PttEventKind.EMERGENCY -> Triple(R.drawable.ic_warning, Ct.Red, "긴급 개시")
        PttEventKind.EMERGENCY_IN -> Triple(R.drawable.ic_warning, Ct.Red, "긴급 수신" + (e.peer?.let { " — $it" } ?: ""))
        PttEventKind.EMERGENCY_END -> Triple(R.drawable.ic_check, Ct.Red, "긴급 해제")
        null -> Triple(R.drawable.ic_history, Ct.TextFaint, e.kind)
    }
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Ct.Surface)
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(34.dp).clip(RoundedCornerShape(10.dp)).background(color.copy(alpha = 0.13f)),
            contentAlignment = Alignment.Center,
        ) {
            Icon(painterResource(icon), contentDescription = null, tint = color, modifier = Modifier.size(16.dp))
        }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text(title, color = Ct.Text, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            Text(st.groupName(e.groupId), color = Ct.TextDim, fontSize = 11.sp,
                modifier = Modifier.padding(top = 2.dp))
        }
        Column(horizontalAlignment = Alignment.End) {
            Text(fmtTime(e.time), color = Ct.TextFaint, fontSize = 11.sp)
            if (e.durationMs > 0) {
                Text(fmtDur(e.durationMs), color = color, fontSize = 11.sp, fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.padding(top = 2.dp))
            }
        }
    }
}
