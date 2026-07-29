"""자동 배포 엔진 — base OAM 내장 (auto_deployment.md).

별도 프로세스가 아니라 OAM 안에서 동작한다: OAM 이 떠 있으면 그 순간부터 쓸 수 있고,
게이트웨이 라우트·별도 포트·인증서·설치 단계가 없다. (엔진을 쓰려면 먼저 엔진을 수동
배포해야 하는 닭-달걀을 없애기 위함.)

  schema     inventory/blueprint 파싱·검증
  ssh        agent 설치용 원격 실행 (비밀번호 인증)
  oam_client OAM REST 클라이언트 (loopback self-call)
  store      블루프린트/인벤토리/run 영속
  engine     phase×step 오케스트레이션 + 체크포인트
  phases/    AGENT → TOPOLOGY → INSTALL → CONFIG → START → VERIFY
"""
