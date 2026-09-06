> 서버(CSP) 요청서 — Windows 관제조작반 실기(2026-09-06 22시, csc 0.2.105 / csp 0.2.112, .45)에서 발견. 서버 반영 후 삭제.
> 설계 정본: [dispatch_center.md](../design/features/dispatch_center.md) §4.5(대표번호 dialog 이벤트), RFC 4235.

# 대표번호 포크 호 — 종료 dialog NOTIFY 귀속·버전 결함

## 1. 현상

관제사 A(disp01, VoLTE `+821310001001`)가 대표번호 `+821310001000` 착신을 받고, **발신자(A-leg, disp02 `+821310001002`)가 BYE** 로 끊으면
CSP 가 보내는 `terminated` dialog-info NOTIFY 세 건의 `entity`·`direction`·`local/remote` 가 confirmed 때와 어긋난다. 관제 앱은 entity|id 로
행을 들고 있으므로 ③ 그룹원 띠·④ 진행 중에 "통화 중" 행이 영구히 남았다(앱 쪽 완화는 §4).

구독자 = disp01 (dialog 구독 3개: 자기 회선 1001 · 동료 1002 · 대표번호 1000). 실측(앱 로그 level 5, NOTIFY 본문 그대로):

| 시각 | entity | version | dialog id | direction | state | local | remote |
|---|---|---|---|---|---|---|---|
| 22:15:28.646 | +821310001000 | 1 | 065218e4 | recipient | early | 1000 | 1002 |
| 22:15:28.650 | +821310001000 | **5** | 065218e4 | recipient | early | 1000 | 1002 |
| 22:15:34.501 | +821310001000 | **2** | 065218e4 | recipient | confirmed | 1000 | 1002 |
| 22:15:34.503 | +821310001001 | 1 | WCSSpnds | recipient | confirmed | 1001 | 1002 |
| 22:15:34.507 | +821310001001 | 2 | WCSSpnds | recipient | confirmed | 1001 | 1002 |
| 22:15:34.507 | +821310001002 | 1 | 065218e4 | initiator | confirmed | 1002 | 1001 |
| 22:15:34.508 | +821310001002 | 3 | 065218e4 | initiator | confirmed | 1002 | 1001 |
| 22:15:45.621 | **+821310001002** | 2 | **WCSSpnds** | recipient | terminated | **1002** | **1000** |
| 22:15:45.626 | +821310001000 | 3 | 065218e4 | **initiator** | terminated | 1000 | 1002 |
| 22:15:45.627 | +821310001000 | 4 | 065218e4 | recipient | terminated | 1000 | **1000** |

(dialog id 앞 8자. `065218e4…` = A-leg(발신자 Call-ID, 대표번호 dialog 와 공유), `WCSSpnds…` = CSP→1001 B-leg Call-ID.)

confirmed 까지는 규격대로다(대표번호 = recipient/remote 1002, 1001 = B-leg recipient, 1002 = initiator/remote 1001). 종료 시:

1. **entity 1001 에는 terminated 가 오지 않는다** — 1001 의 WCSS dialog 종료가 entity **1002** 로 나갔다(1002 는 WCSS dialog 의 당사자가 아니고,
   local/remote 도 1002/1000 으로 뒤바뀜).
2. entity 1000 에 같은 dialog(065218e4) 의 terminated 가 두 번, 한 번은 `direction="initiator"`(대표번호는 항상 recipient), 한 번은 `remote=1000`(자기 자신).
3. **version 이 구독별 단조 증가가 아니다** — 대표번호 구독의 version 이 1, 5, 2, 3, 4 순으로 왔고, 1001/1002 구독에는 같은 상태의 NOTIFY 가 두 번
   (version 1·2, 1·3) 갔다. RFC 4235 §4.1: version 은 구독마다 1씩 증가, 구독자는 작은 version 을 버려야 하므로 지금 순서면 규격 준수 단말은
   대표번호의 confirmed(2)·terminated(3,4)를 **전부 버린다**(early 의 version 5 뒤라서).

**동료가 받고 착신측(B-leg, 포크 승자)이 BYE** 한 경우(22:27:50 실측)는 세 entity 모두 정상(같은 id 로 confirmed→terminated, direction·remote 일치).
즉 결함은 **A-leg BYE 처리 경로**에서 종료 NOTIFY 를 만들 때 leg/entity 를 잘못 매기는 것으로 보인다(포크 집합(`CTasForkSet`)의 승자 leg 와 A-leg 를
한 dialog 로 묶어 대표번호에 내보내는 §4.5 경로).

## 2. 요청 (RFC 4235 정합)

- 각 dialog 구독(entity)에는 **그 entity 가 당사자인 dialog** 만, `local`=entity 자신, `remote`=상대, `direction` 은 early/confirmed 때와 같게.
  - entity 1001(포크 승자): WCSS `recipient` terminated, remote 1002.
  - entity 1002(발신자): 065218e4 `initiator` terminated, remote 1001(승자 확정 뒤 remote 는 승자).
  - entity 1000(대표번호): 065218e4 `recipient` terminated, remote 1002 — **한 번만**.
- `version` 은 구독(entity)마다 1씩 단조 증가(같은 상태 중복 발송 없음). 대표번호 dialog 의 early 가 두 번(1→5) 나가는 것도 같은 원인으로 보인다.
- 검증 = `S3-SCN-FA` 에 "A-leg BYE 뒤 세 구독의 terminated entity/direction/remote 일치 + version 단조" 판정 추가 권고.

## 3. 재현

```bash
# 관제 앱(또는 cimsue-cli dialog-watch 3개) 을 disp01 로 두고
cimsue-cli --csc-host 121.161.164.45 --csc-port 4430 --no-tls-verify --user disp02 --pw 1234 --from-profile volte \
           --json call +821310001000 --duration 10        # disp01 이 응답 → 10초 뒤 disp02(A-leg) 가 BYE
```

## 4. 앱 쪽 완화(반영됨, 커밋 참조)

dialog id(Call-ID+태그)는 양 당사자에게 같은 하나의 dialog 이므로 앱은 `terminated` 를 **entity 와 무관하게 그 id 의 행 전부**에 적용한다
(`DispatchSession.OnDialog`). 이 규칙으로 위 결함에서도 잔류 행은 없어지지만, version 무시·entity 오귀속은 앱이 고칠 수 없으니 서버 수정이 필요하다.
