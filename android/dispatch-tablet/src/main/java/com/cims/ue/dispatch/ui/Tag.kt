// 작은 라벨(태그) — 한 곳에서만 그린다 (android_dispatch_tablet.md §12).
//
// 같은 모양·같은 뜻의 라벨이 화면마다 따로 정의돼 있었다(`Chip`·`Tag`·`Badge` 네 벌). 모양은 같은데 배경
// 투명도만 0.16·0.20·0.22 로 달라, 같은 화면 안에서 옮겨 다닐 때 «같은 것인가» 가 흔들렸다. 한 벌로 모은다.
package com.cims.ue.dispatch.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/**
 * 상태·분류를 한 낱말로 붙이는 라벨. 누를 수 없다 — 누르는 것은 칩·알약 버튼이다.
 *
 * 색조가 정해진 상태(긴급·임박·통화 중·부재·청취·감청·대표…)는 [style] 로 준다 — [Label] 과 같은 토큰이다.
 *
 * @param leading 앞에 두는 간격. 이름 뒤에 붙일 때 4dp, 문장 뒤면 0.
 */
@Composable
fun Tag(text: String, style: LabelStyle = LabelStyle.FILL, leading: Int = 4) {
    Label(text, style, Modifier.padding(start = leading.dp))
}

/**
 * 색을 직접 주는 라벨 — 토큰에 없는 색(레인 색처럼 값에서 나온 것)에만 쓴다. 배경은 그 색의 옅은 면(단일 투명도).
 */
@Composable
fun Tag(text: String, color: Color, leading: Int = 4) {
    Surface(color = color.copy(alpha = 0.16f), shape = RoundedCornerShape(4.dp),
        modifier = Modifier.padding(start = leading.dp)) {
        Text(text, Modifier.padding(horizontal = 5.dp, vertical = 1.dp),
            fontSize = Type.micro, color = color, fontWeight = androidx.compose.ui.text.font.FontWeight.Bold)
    }
}
