"""AuC — IMS AKA 인증 벡터 발급자 (sip_access_security.md §8.2, TS 33.203 Annex X / TS 33.102 §6.3).

CSC 가 HSS/AuC 역할을 맡는다: K/OPc 를 암호화 보관하고, CSP 의 요청에 AV(RAND, AUTN, XRES, CK, IK)
만 발급한다. SQN 증가·AUTS 재동기는 이 패키지 안에서만 일어난다(단일 쓰기 주체).

  aes128    — 순수 python AES-128 블록 암호(외부 의존 없음 — vendor 에 crypto 라이브러리가 없다)
  milenage  — TS 35.205/206 f1/f1*/f2/f3/f4/f5/f5*
  keystore  — K/OPc 보관 형식(AES-128-CTR + HMAC-SHA256, KEK = csc.json AuC.Kek)
  auc       — AV 발급·AUTS 재동기 (DB 트랜잭션)
"""
