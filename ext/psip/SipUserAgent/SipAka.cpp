#include "SipAka.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#include "Base64.h"

static bool AesEncryptBlock( const std::string &strKey, const std::string &strIn, std::string &strOut ) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if ( ctx == NULL ) return false;
    unsigned char out[32];
    int iLen = 0, iTotal = 0;
    bool bOk = EVP_EncryptInit_ex( ctx, EVP_aes_128_ecb(), NULL, (const unsigned char *)strKey.data(), NULL ) == 1;
    if ( bOk ) EVP_CIPHER_CTX_set_padding( ctx, 0 );
    if ( bOk ) bOk = EVP_EncryptUpdate( ctx, out, &iLen, (const unsigned char *)strIn.data(), 16 ) == 1;
    iTotal = iLen;
    if ( bOk ) bOk = EVP_EncryptFinal_ex( ctx, out + iTotal, &iLen ) == 1;
    iTotal += iLen;
    EVP_CIPHER_CTX_free( ctx );
    if ( !bOk || iTotal != 16 ) return false;
    strOut.assign( (const char *)out, 16 );
    return true;
}

static std::string Xor( const std::string &a, const std::string &b ) {
    std::string o( a.size(), '\0' );
    for ( size_t i = 0; i < a.size() && i < b.size(); ++i ) o[i] = a[i] ^ b[i];
    return o;
}

static std::string Rot( const std::string &s, int iBits ) {
    const size_t n = ( iBits / 8 ) % 16;
    return s.substr( n ) + s.substr( 0, n );
}

bool SipAkaHexToBytes( const std::string &strHex, std::string &strOut ) {
    if ( strHex.size() % 2 ) return false;
    strOut.clear();
    for ( size_t i = 0; i < strHex.size(); i += 2 ) {
        unsigned int v = 0;
        if ( sscanf( strHex.substr( i, 2 ).c_str(), "%2x", &v ) != 1 ) return false;
        strOut.push_back( (char)v );
    }
    return true;
}

// TS 35.206 §4: TEMP = E_K(RAND⊕OPc); OUTn = E_K(rot(TEMP⊕OPc, rn) ⊕ cn) ⊕ OPc
//   r1..r5 = 64,0,32,64,96 / c1..c5 = 0,1,2,4,8 (마지막 바이트)
void SipAkaMilenage( const std::string &strK, const std::string &strOpc, const std::string &strRand,
                     const std::string &strSqn, const std::string &strAmf, std::string &strMacA,
                     std::string &strMacS, std::string &strRes, std::string &strCk, std::string &strIk,
                     std::string &strAk, std::string &strAkStar ) {
    std::string strTemp;
    AesEncryptBlock( strK, Xor( strRand, strOpc ), strTemp );

    // f1 / f1*
    std::string strIn1 = strSqn + strAmf + strSqn + strAmf;
    std::string strOut1;
    AesEncryptBlock( strK, Xor( strTemp, Rot( Xor( strIn1, strOpc ), 64 ) ), strOut1 );
    strOut1 = Xor( strOut1, strOpc );
    strMacA = strOut1.substr( 0, 8 );
    strMacS = strOut1.substr( 8, 8 );

    std::string strBase = Xor( strTemp, strOpc );
    std::string strX, strOut;

    // f2 / f5
    strX = Rot( strBase, 0 ); strX[15] ^= 1;
    AesEncryptBlock( strK, strX, strOut ); strOut = Xor( strOut, strOpc );
    strRes = strOut.substr( 8, 8 );
    strAk = strOut.substr( 0, 6 );
    // f3
    strX = Rot( strBase, 32 ); strX[15] ^= 2;
    AesEncryptBlock( strK, strX, strOut ); strCk = Xor( strOut, strOpc );
    // f4
    strX = Rot( strBase, 64 ); strX[15] ^= 4;
    AesEncryptBlock( strK, strX, strOut ); strIk = Xor( strOut, strOpc );
    // f5*
    strX = Rot( strBase, 96 ); strX[15] ^= 8;
    AesEncryptBlock( strK, strX, strOut ); strAkStar = Xor( strOut, strOpc ).substr( 0, 6 );
}

static std::string SqnToBytes( uint64_t iSqn ) {
    std::string s( 6, '\0' );
    for ( int i = 5; i >= 0; --i ) {
        s[i] = (char)( iSqn & 0xff );
        iSqn >>= 8;
    }
    return s;
}

static uint64_t SqnFromBytes( const std::string &s ) {
    uint64_t v = 0;
    for ( size_t i = 0; i < 6 && i < s.size(); ++i ) v = ( v << 8 ) | (unsigned char)s[i];
    return v;
}

bool SipAkaCompute( const std::string &strKHex, const std::string &strOpcHex, const std::string &strNonceB64,
                    uint64_t &iSqnMs, CSipAkaResult &clsOut ) {
    std::string strK, strOpc;
    if ( !SipAkaHexToBytes( strKHex, strK ) || !SipAkaHexToBytes( strOpcHex, strOpc ) ) return false;
    if ( strK.size() != 16 || strOpc.size() != 16 ) return false;

    std::string strNonce( GetBase64DecodeLength( (int)strNonceB64.size() ) + 1, '\0' );
    const int iLen = Base64Decode( strNonceB64.c_str(), (int)strNonceB64.size(), &strNonce[0], (int)strNonce.size() );
    if ( iLen < 32 ) return false;
    const std::string strRand = strNonce.substr( 0, 16 );
    const std::string strAutn = strNonce.substr( 16, 16 );
    const std::string strSqnXorAk = strAutn.substr( 0, 6 );
    const std::string strAmf = strAutn.substr( 6, 2 );
    const std::string strMac = strAutn.substr( 8, 8 );

    std::string strMacA, strMacS, strRes, strCk, strIk, strAk, strAkStar;
    SipAkaMilenage( strK, strOpc, strRand, std::string( 6, '\0' ), strAmf, strMacA, strMacS, strRes, strCk, strIk,
                    strAk, strAkStar );
    const std::string strSqn = Xor( strSqnXorAk, strAk );
    // MAC-A 는 실제 SQN 으로 다시 계산한다 (위 호출은 AK 를 얻기 위한 것)
    SipAkaMilenage( strK, strOpc, strRand, strSqn, strAmf, strMacA, strMacS, strRes, strCk, strIk, strAk, strAkStar );

    clsOut.iSqn = SqnFromBytes( strSqn );
    clsOut.bMacOk = ( strMacA == strMac );
    clsOut.strRes = strRes;
    clsOut.strCk = strCk;
    clsOut.strIk = strIk;
    if ( !clsOut.bMacOk ) return true;

    // TS 33.102 §6.3.3 / Annex C.2 — 단순 규칙: SQN 이 SQN_MS 보다 커야 신선하다.
    clsOut.bSqnOk = ( clsOut.iSqn > iSqnMs );
    if ( clsOut.bSqnOk ) {
        iSqnMs = clsOut.iSqn;
        return true;
    }
    // 재동기: AUTS = (SQN_MS ⊕ AK*) ‖ MAC-S,  MAC-S = f1*(K, RAND, SQN_MS, AMF*=0000)
    const std::string strSqnMs = SqnToBytes( iSqnMs );
    std::string strMacA2, strMacS2, strRes2, strCk2, strIk2, strAk2, strAkStar2;
    SipAkaMilenage( strK, strOpc, strRand, strSqnMs, std::string( 2, '\0' ), strMacA2, strMacS2, strRes2, strCk2,
                    strIk2, strAk2, strAkStar2 );
    const std::string strAuts = Xor( strSqnMs, strAkStar2 ) + strMacS2;
    Base64Encode( strAuts.data(), (int)strAuts.size(), clsOut.strAutsB64 );
    return true;
}
