#ifndef _MD5_H_
#define _MD5_H_

void SipMd5String( const char * pszPlainText, char result[33] );
void SipMd5Byte( const char * pszPlainText, unsigned char digest[16] );
/** 길이 지정 입력(이진 포함)의 MD5 hex(32) — RFC 3310 AKA 의 H(A1) 처럼 NUL 을 포함할 수 있는 입력용 */
void SipMd5Buffer( const unsigned char * pszInput, int iLen, char szMd5[33] );

#endif
