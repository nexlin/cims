/*
 * SipAka — 단말측 IMS AKA 응답 계산 (Milenage TS 35.206 + RFC 3310 AKAv1-MD5)
 *
 * 소프트-USIM: K/OPc 를 hex 로 받아 챌린지 nonce=base64(RAND‖AUTN) 에서 RES/CK/IK 를 만들고,
 * AUTN 의 MAC 검증과 SQN 신선도 검사(TS 33.102 §6.3.3 — 단조 증가 단순 규칙)를 수행한다.
 * SQN 이탈이면 AUTS=(SQN_MS⊕AK*)‖MAC-S(AMF*=0000) 를 만든다(§6.3.5).
 */
#ifndef _SIP_AKA_H_
#define _SIP_AKA_H_

#include <stdint.h>

#include <string>

struct CSipAkaResult {
    bool bMacOk = false;    // AUTN MAC-A == f1(K, RAND, SQN, AMF)
    bool bSqnOk = false;    // SQN > SQN_MS (신선)
    std::string strRes;     // 8B (이진) — AKAv1-MD5 의 password
    std::string strCk;      // 16B
    std::string strIk;      // 16B
    std::string strAutsB64; // bMacOk && !bSqnOk 일 때 — auts 파라미터 값
    uint64_t iSqn = 0;      // 챌린지의 SQN (AK 제거 후)
};

/** hex 문자열 → 바이트. 길이/문자 오류면 false */
bool SipAkaHexToBytes( const std::string &strHex, std::string &strOut );

/**
 * @brief 챌린지 nonce 를 풀어 Milenage 로 답한다.
 * @param strKHex   K (hex32)   @param strOpcHex  OPc (hex32)
 * @param strNonceB64 WWW-Authenticate nonce (base64(RAND‖AUTN), 32B)
 * @param iSqnMs    [in/out] 단말의 SQN_MS — bMacOk && bSqnOk 면 챌린지 SQN 으로 갱신된다
 * @returns nonce/키 형식 오류면 false
 */
bool SipAkaCompute( const std::string &strKHex, const std::string &strOpcHex, const std::string &strNonceB64,
                    uint64_t &iSqnMs, CSipAkaResult &clsOut );

/** Milenage 커널 — 시험 하네스용 공개. 모든 인자는 이진 문자열(K/OPc/RAND 16B, SQN 6B, AMF 2B). */
void SipAkaMilenage( const std::string &strK, const std::string &strOpc, const std::string &strRand,
                     const std::string &strSqn, const std::string &strAmf, std::string &strMacA,
                     std::string &strMacS, std::string &strRes, std::string &strCk, std::string &strIk,
                     std::string &strAk, std::string &strAkStar );

#endif
