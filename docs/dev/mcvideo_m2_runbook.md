# MCVideo M2 — .48 배포·준비 절차 (서버 단독 신호 시험)

[mcvideo_dev_plan.md](mcvideo_dev_plan.md) §5 M2 를 .48 에서 여는 절차다. **실행은 사용자 결정 셋 뒤** — ① .48 에 CSP·CMP·CSC 배포 ② 공유 DB
`sql/migrate_mcvideo.sql` ③ .48 CSP `Setup.Roles.MCVIDEO` 켜기. 시험 항목(T1~T9)·UE 명령은 [server45_handoff.md](server45_handoff.md) §11
«M2 신호 시험 절차 제안»·«.48 답 — M2 신호 시험 준비» 가 정본이고, 이 문서는 .48 쪽 준비만 적는다. 배포 방식 = OAM API
([oam_api_deploy_runbook.md](oam_api_deploy_runbook.md) — 콘솔을 쓰지 않는다).

## 0. 무엇이 들어가나

| 모듈 | 내용 | 설계 |
|---|---|---|
| CSP | A7~A11 — MCVideo 모듈·등록 능력·서비스별 affiliation·그룹 호(`McVideoCallService`)·CMP MCVideo 명령·미디어 SRTP(m= 라인마다) | [mcvideo.md](../design/features/mcvideo.md) §5.2·§5.2.1 |
| CMP | B3~B7 — MCVideo 그룹 종류·멤버 6포트 블록·전송/수신 제어 상태 머신·분배·SRTP/SRTCP·영상 RTCP 키프레임 요청(B6) | §5.3·§5.3.1 |
| CSC | A1~A6 — 설정 평면(그룹 문서 MCVideo 몫·CMS 문서·ue-init-config·scope)·관리 API(그룹 `mcvideo`·PTT 회선 자격) | §5.1 · [admin_api.md](../api/admin_api.md) §5.4·§6 |

배포 id(.48) = csc 3 · cmp 4 · csp 6. `.45` 라이브 CSP·CSC 는 MCVideo 코드가 없어 새 표를 읽지 않는다 — 공유 DB 에 표를 더해도 무영향.

## 1. 공유 DB 마이그레이션 (결정 ②)

`sql/migrate_mcvideo.sql` — 표 추가만(`mcvideo_group_attrs`·`mcvideo_user_profile`·`mcvideo_affiliations`), 재실행 안전. DB 는 .45:3306/cims.
.45 에서 `sudo mysql cims < sql/migrate_mcvideo.sql`, 또는 .48 에서 배포 CSC 의 `CimsDatabase` 자격으로 pymysql 실행(vendor =
`/opt/cims-agent/modules/csc/current/csc/vendor`).

- 이 스크립트는 `video_enabled=1` 그룹(g004 등)에 MCVideo 속성 행을, **PTT 회선 전부**에 MCVideo 자격 행을 넣는다(현행 «PTT 영상» 보존 —
  mcvideo.md §8). .48 CSC 가 발급하는 토큰에만 `3gpp:mc:video_*`·`mcvideo_id` 가 붙는다. 시험 신원만 자격을 두려면 적용 뒤 §3 의 A6 `DELETE` 로
  거둔다(사용자 결정).
- 확인: `SHOW TABLES LIKE 'mcvideo_%'` = 3.

## 2. 배포 (결정 ①·③)

```bash
cd build && make -j$(nproc) && make dist && cd ..
./cims.sh pkg csp cmp csc                                   # auto-bump → build/dist/packages/*.tar.gz
scripts/oam-deploy.py packages build/dist/packages/csp-<v>.tar.gz build/dist/packages/cmp-<v>.tar.gz build/dist/packages/csc-<v>.tar.gz
scripts/oam-deploy.py upgrade 4=<cmp pkg> 3=<csc pkg> 6=<csp pkg>    # CMP 먼저(CSP 가 HEARTBEAT 로 resource.mcvideo 를 배운다)
```

설정 overlay:

| 배포 | 키 | 값 | 비고 |
|---|---|---|---|
| csp(6) | `Setup.Roles.MCVIDEO` | `true` | restart — 끄면 MCVideo 요청은 404(MCPTT 로 읽지 않는다) |
| csc(3) | `UeInitConfig.ServiceDetails.McVideo.Enable` | `true` | ue-init-config 에 `MCVideo-Service-Details`(기본 PSI `sip:mcvideo_psi@<PTT 도메인>`). cli 가 `--mcvideo-psi` 를 주면 없어도 된다 |
| cmp(4) | — | — | 기본값 그대로: `McVideoMemberPoolSize` 40 · `McVideoStartPort` 59000(멤버당 6포트 → 59000~59239). .48 기존 범위(50000~·52000~·54000~·56000~·58000~)와 겹치지 않는다 |

```bash
scripts/oam-deploy.py config 6 --file <(echo '{"Setup.Roles.MCVIDEO": true}') --restart
scripts/oam-deploy.py config 3 --file <(echo '{"UeInitConfig.ServiceDetails.McVideo.Enable": true}') --restart
```

확인:
- CMP 로그 `MCVideo member pool` 이 비활성 문구가 아닐 것, `STATS` 에 `mcvideo_groups` 키.
- CSP 로그 기동 줄 `ModuleDispatcher: Roles … MCVIDEO-AS=ON` · 첫 MCVideo INVITE 가 500(`CMP resource.mcvideo 없음`)이면 CMP 가 먼저 떠 있지 않았다.
- 방화벽이 있으면 UDP 59000~59239 를 .45 쪽으로 연다.

## 3. 시험 준비 — A6 관리 API (OAM 게이트웨이 경유)

신원 = 계측기 PTT 신원 A `+82500000023` · B `+82500000024` · C `+82500000025`(그룹 없음 — 로그인 자격은
`/mnt/cims/test48/tester/scenarios/creds/ptt.jsonl`). M2 동안 계측기는 이 신원으로 돌리지 않는다.

1. 가입자 id 찾기 — `GET /api/v1/users` 에서 세 번호의 `ptt_subscriptions[].id` 를 가진 가입자(person id).
2. 자격 확인 — `GET /api/v1/users/{pid}/ptt/{msisdn}/mcvideo` 가 200(마이그레이션이 넣었다). 없으면 `PUT … {}`(기본 C9 1·N6 1).
3. 그룹 둘:

```json
POST /api/v1/ptt/groups
{"id": "gmv1", "name": "MCVideo 시험 chat", "group_type": "chat",
 "members": [{"user_id": "+82500000023", "priority": 5}, {"user_id": "+82500000024", "priority": 3}, {"user_id": "+82500000025", "priority": 1}],
 "mcvideo": {"invite_members": false, "max_transmitters": 1}}

POST /api/v1/ptt/groups
{"id": "gmv2", "name": "MCVideo 시험 prearranged", "group_type": "prearranged", "hang_timer_sec": 10,
 "members": [{"user_id": "+82500000023", "priority": 5}, {"user_id": "+82500000024", "priority": 3}, {"user_id": "+82500000025", "priority": 1}],
 "mcvideo": {"invite_members": true}}
```

   `GET /api/v1/ptt/groups/gmv1` 의 `mcvideo` 가 객체인지 본다. CSC 가 CSP 에 `GROUP_CHANGED` 를 보내 CSP 그룹 맵에 곧바로 들어간다.
   gmv2 의 `hang_timer_sec` 10 은 T9(T1 만료 해제)용 — 끝나면 기본값으로 되돌려도 된다.
4. dev_share 로 .45 에 준비 끝을 알린다(T1~T9 실행은 .45 cli).

## 4. 관측

- CSP 로그 `MCVIDEO:` 줄 — session start/end·accept·invite·joined·left·(re-INVITE)·SRTP 판정. 호마다 `sesid` 로 CMP 로그와 잇는다.
- CMP `STATS` `detail.mcvideo_groups[]` — members·reserved·transmitters·receptions·control_rx·no_grant_drop·crypto_drop·keyframe_requests.
- DB — `mcvideo_affiliations`(T2 에서 행이 생기고 등록 해제로 지워짐) · **`ptt_affiliations` 무변화**.

## 5. 되돌리기

- `Setup.Roles.MCVIDEO` false + restart — MCVideo 요청은 다시 404. 진행 중 MCVideo 호는 CSP 재기동으로 끝난다.
- 모듈 롤백 = `POST /api/v1/deployments/{id}/rollback`(csp·csc·cmp). 표는 남겨도 옛 코드가 읽지 않는다.
- 시험 그룹 삭제 = `DELETE /api/v1/ptt/groups/gmv1`·`gmv2`(FK CASCADE 로 MCVideo 속성·affiliation 행도 지워진다).
