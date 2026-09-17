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
internal fun userPart(uri: String): String =
    uri.trim().substringAfter(':', uri.trim()).substringBefore('@').substringBefore(';').trim()
