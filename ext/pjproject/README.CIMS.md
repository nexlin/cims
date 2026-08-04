# ext/pjproject — CIMS 안드로이드 UE 패치 적용 pjproject 소스

pjsip/pjproject **2.16** (upstream commit `6cab30c`) 트래킹 소스 전체에
[android/docs/scripts/m1_build_pjsip.sh](../../android/docs/scripts/m1_build_pjsip.sh)
패치 **[2-3]~[2-13]** 을 적용한 상태의 스냅샷이다. psip·opencore-amr 등과 같이
"수정해서 쓰는 외부 소스는 ext/ 에 커밋"하는 관례를 따른다.

## 정본(SoT)과의 관계

- **패치의 정본은 `m1_build_pjsip.sh`** 다. 안드로이드 `.so` 빌드는 여전히 이
  스크립트가 upstream 2.16 을 clone 하고 패치를 적용하는 경로로 수행한다
  ([docs/design/features/android_ue_m1_pjsip_integration.md](../../docs/design/features/android_ue_m1_pjsip_integration.md)).
- 이 트리는 그 적용 결과의 **백업/열람용 스냅샷**이다 — pjproject 쪽 수정 내용을
  diff 조각이 아닌 실제 소스 형태로 레포에서 바로 볼 수 있게 한다.
- **[2-14] (U10 SSRC 디먹스, stream.c)** 는 이 트리에 **미적용** — 스크립트에만
  존재하며 WSL2 빌드·실기기 검증 후 반영한다.

## 갱신 절차

pjproject 를 수정할 때:

1. `m1_build_pjsip.sh` 에 패치 단계(`[2-N]`)를 추가한다 (정본 갱신).
2. 패치 적용된 작업 트리(예: `/home/cims/pjproject`)에서 이 디렉토리를 재생성한다:

   ```bash
   cd <patched-pjproject>
   git ls-files -z | tar --null -cf - -T - | tar -xf - -C <cims>/ext/pjproject
   ```

3. 두 변경을 같은 커밋으로 올린다.

> `.gitignore` 의 `**/build/`·`**/Makefile` 패턴에 걸리는 파일이 있으므로
> 새 파일 추가 시 `git add -f ext/pjproject` 를 쓴다 (기존 트래킹 파일은 무관).
