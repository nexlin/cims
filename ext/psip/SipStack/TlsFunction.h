/* 
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com> (http://blog.naver.com/websearch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */

#ifndef _TLS_FUNCTION_H_
#define _TLS_FUNCTION_H_

#ifdef USE_TLS

#include "SipTcp.h"
#include "openssl/rsa.h"
#include "openssl/crypto.h"
#include "openssl/x509.h"
#include "openssl/pem.h"
#include "openssl/ssl.h"
#include "openssl/err.h"

bool SSLStart( );

bool SSLServerStart( const char * szCertFile, const char * szCaCertFile );
bool SSLServerStop( );

bool SSLClientStart( );
bool SSLClientStop( );

void SSLFinal();

// ── R5.c: per-listener SSL_CTX ─────────────────────────
/** 독립 SSL_CTX 생성 + cert/key 로드. 성공 시 새 SSL_CTX 반환, 실패 시 NULL.
 *  szKeyFile 이 NULL 또는 빈 문자열이면 szCertFile 에서 key 도 로드 (combined PEM).
 *  szCaCertFile 이 유효하면 client cert 검증 활성화 (mTLS).
 *  호출자가 SSLServerCtxFree() 로 해제 책임. */
SSL_CTX * SSLServerCtxCreate( const char * szCertFile, const char * szKeyFile, const char * szCaCertFile );
void SSLServerCtxFree( SSL_CTX * ctx );

/** 지정된 ctx 로 accept. ctx 가 NULL 이면 기본 global server ctx 사용. */
bool SSLAcceptWithCtx( Socket iFd, SSL_CTX * ctx, SSL ** ppsttSsl, bool bCheckClientCert, int iVerifyDepth, int iAcceptTimeout );

bool SSLConnect( Socket iFd, SSL ** ppsttSsl );
bool SSLAccept( Socket iFd, SSL ** ppsttSsl, bool bCheckClientCert, int iVerifyDepth, int iAcceptTimeout );
int SSLSend( SSL * ssl, const char * szBuf, int iBufLen );
int SSLRecv( SSL * ssl, char * szBuf, int iBufLen );
bool SSLClose( SSL * ssl );

#ifdef WIN32
void SSLPrintLogServerCipherList( );
void SSLPrintLogClientCipherList( );
#endif

#else

#define SSL void

#endif

#endif
