# volte-client — VoLTE 1:1 SIP 소프트폰

CSP에 SIP로 등록하고 1:1 음성/영상 통화를 수행하는 안드로이드 전화 앱.
("VoLTE" = 통신사 IMS 무선 연동이 아니라 **CSP를 향한 SIP 소프트폰**)

## 범위

- REGISTER (Digest MD5 / qop=auth) · INVITE/SDP · BYE · 호 상태머신
- AMR-WB (PT 99, `octet-align=1; mode-set=0,1,2`) 양방향 RTP, AEC/지터버퍼(PJSIP)
- (후속) H.264 영상, 서버측 기능 연동(DND·착신전환 등)

## 내부 구성 (계획)

`app`(Compose UI / Foreground Service / ViewModel) — SIP/미디어/PJSIP/코덱은 공유 **`core`** 모듈에 의존.

## 상태

스캐폴드 대기 (M0 → M1). 설계: [../../docs/design/features/android_ue_client.md](../../docs/design/features/android_ue_client.md) (마일스톤 M1)
