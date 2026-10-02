// URI → 번호 — 한 곳에서만 자른다.
//
// 같은 일을 하는 함수가 여섯 벌 있었고(`userPart`·`userPartOf`·`userPartOfUri`), **셋은 서로 다르게 잘랐다** —
// 어떤 것은 `;user=phone` 파라미터를 떼고 어떤 것은 두었으며, 어떤 것은 공백을 다듬고 어떤 것은 두었다.
// 이 값은 «같은 사람인가» 를 판정하는 열쇠라(로스터·주소록·감시 대상 대조), 자르는 법이 갈라지면
// 같은 사람이 두 사람이 된다. 한 벌로 모은다.
package com.cims.ue.dispatch.session

/**
 * `sip:1001@dom;user=phone` · `tel:+8210…` · `1001` → 번호 부분.
 *
 * 스킴·호스트·파라미터를 떼고 앞뒤 공백을 다듬는다. 스킴이 없으면 입력을 그대로 본다.
 * 표기 차이(`010…` vs `+8210…`)까지 맞추려면 그 위에 [DirectoryBook.normalize] 를 건다.
 */
internal fun userPart(uri: String): String {
    // name-addr(`"표시 이름" <sip:…>`)이면 꺾쇠 안만 본다 — 표시 이름에 든 `@`·`:` 에 흔들리지 않는다
    val t = uri.trim().let { if ('<' in it && '>' in it.substringAfter('<')) it.substringAfter('<').substringBefore('>').trim() else it }
    val at = t.indexOf('@')
    // 스킴의 `:` 은 `@` **앞의 마지막** 것이다 — `1001@host:5060` 의 `:` 은 포트고(스킴으로 읽으면 번호가 «5060» 이 된다), 표시
    //   이름에 든 `:` 도 스킴이 아니다. `@` 가 없으면(`tel:`·맨 번호) 첫 `:` 이다.
    val colon = if (at >= 0) t.lastIndexOf(':', at) else t.indexOf(':')
    return (if (colon >= 0) t.substring(colon + 1) else t).substringBefore('@').substringBefore(';').trim()
}
