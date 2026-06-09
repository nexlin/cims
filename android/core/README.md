# core — 공유 라이브러리 (Android Library)

`volte-client`·`ptt-client`가 함께 의존하는 공유 모듈. VoLTE·PTT의 무거운 공통 부분(PJSIP 통합·코덱·SIP/미디어)을 한 곳에 두어 중복·이중 유지보수를 제거한다.

## 포함 (공유)

- **PJSIP 통합**: 빌드 산출물(`libpjsua2.so` 등) + SWIG Java(`org.pjsip.pjsua2.*`)
- **SipController**: REGISTER(Digest MD5/qop=auth) · INVITE/SDP · BYE · 호 상태머신 (pjsua2 래퍼)
- **MediaCodec 코덱 팩토리**: AMR-WB(음성, 커스텀 `pjmedia_codec_factory`) · H.264(영상)
- **MediaControl**: 오디오 장치/AEC/지터버퍼 설정, conference bridge 연결
- 공통 모델/유틸(SDP 파싱 헬퍼, 설정)

## 제외 (클라이언트별)

- floor(MCPT RTCP-APP) · affiliation · group · CSC(OAuth2 PKCE+XCAP) → **ptt-client**
- 각 앱 UI / Foreground Service / ViewModel → 각 클라이언트

## 형태

Android Library 모듈(`com.android.library`). `volte-client`/`ptt-client`가 `implementation project(':core')` 로 의존.

## 상태

스캐폴드 대기(M0). 설계: [../../docs/design/features/android_ue_client.md](../../docs/design/features/android_ue_client.md) §9
