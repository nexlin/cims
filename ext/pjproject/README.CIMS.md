# ext/pjproject — CIMS 단말 엔진 pjproject (소스 정본)

pjsip/pjproject **2.16** (upstream commit `6cab30c`) 에 CIMS 단말(UE) 패치를 적용한 소스 트리.
psip·opencore-amr 처럼 "수정해서 쓰는 외부 소스는 `ext/` 에 커밋"하는 관례를 따르며,
**단말 SDK `libcimsue` 의 엔진**으로 Linux(개발 서버·`cimsue-cli`)·Android(NDK)·Windows(MSVC)
세 툴체인이 **이 트리 하나**를 빌드한다 — 설계 정본 [docs/design/features/ue_sdk.md](../../docs/design/features/ue_sdk.md) §3.

## 정본(SoT)

- **이 트리가 유일한 소스 정본이다.** pjproject 를 수정할 때는 여기서 직접 고치고 커밋한다. 변경 이력은 git.
- 적용된 CIMS 패치의 내용과 이유는 각 수정 지점의 `CIMS` 주석이 설명한다(예: `pjmedia/src/pjmedia/stream.c`
  의 U10 SSRC 디먹스, `pjsip/src/pjsua-lib/pjsua_pres.c` 의 `cims_conf_*` 이벤트 구독). 패치 인벤토리(AMR-WB
  codec_setting·H.264 IDR/비트레이트·StreamInfo 가드·m=text 슬롯 스킵·무전/통화 분리 라우팅·AMR 인코더 워치독·
  PTT 유휴 무음 제거·conference/xcap-diff 구독·U10)는 [android/docs/scripts/m1_build_pjsip.sh](../../android/docs/scripts/m1_build_pjsip.sh)
  의 `[2-N]` 단계 제목이 목록 역할을 한다.
- `config_site.h` 는 upstream 이 무시하는 파일이라 트리에 없다. 플랫폼별 정본은 `sdk/engine/config_site/`
  (ue_sdk.md §3) 이며 빌드 시 `pjlib/include/pj/config_site.h` 로 복사한다.

## 빌드

| 플랫폼 | 방법 |
|---|---|
| Linux | 루트 CMake `ExternalProject_Add(pjproject)` — `aconfigure` + `make` (ue_sdk.md §8) |
| Android | `m1_build_pjsip.sh` — 이 트리를 `configure-android`(NDK) 로 빌드 + SWIG. 스크립트의 upstream clone·패치 적용 단계는 ue_sdk.md §10 A 단계에서 제거된다(트리가 정본이므로 재적용 대상이 없다) |
| Windows | `pjproject-vs14.sln` (MSVC) |

## 경계

- `third_party/srtp`(동봉분)는 단말 엔진 전용이다. 서버(CMP)는 `ext/libsrtp` 독립 vendoring 을 쓰며 서로 링크하지 않는다
  (루트 `CMakeLists.txt` libsrtp 절).
- upstream 갱신은 이 트리 위에서 merge/rebase 로 한다.

> `.gitignore` 의 `**/build/`·`**/Makefile` 패턴에 걸리는 파일이 있으므로 새 파일 추가 시 `git add -f ext/pjproject` 를 쓴다
> (기존 트래킹 파일은 무관).
