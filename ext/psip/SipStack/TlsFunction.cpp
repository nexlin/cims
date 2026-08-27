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

#include "SipStackDefine.h"

#ifdef USE_TLS

#include "TlsFunction.h"
#include "SipMutex.h"
#include "Log.h"
#include "FileUtility.h"
#include "MemoryDebug.h"

static SSL_CTX	* gpsttServerCtx = NULL;
static SSL_CTX	* gpsttClientCtx = NULL;

/** gpsttServerCtx 교체·참조 획득을 직렬화한다 — 무중단 인증서 교체(SSLServerCtxReload)가
 *  accept 스레드와 동시에 일어나기 때문이다. 핸드셰이크당 한 번 잠그는 비용은 무시할 수준. */
static CSipMutex gclsServerCtxMutex;

/** SSL_CTX 참조 카운트 증가 (OpenSSL 1.1 이전은 CRYPTO_add). */
static void _SslCtxUpRef( SSL_CTX * psttCtx )
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	SSL_CTX_up_ref( psttCtx );
#else
	CRYPTO_add( &psttCtx->references, 1, CRYPTO_LOCK_SSL_CTX );
#endif
}

#if OPENSSL_VERSION_NUMBER >= 0x10000003L
static const SSL_METHOD	* gpsttServerMeth;
static const SSL_METHOD * gpsttClientMeth;
#else
static SSL_METHOD	* gpsttServerMeth;
static SSL_METHOD * gpsttClientMeth;
#endif

static bool gbStartSslServer = false;
static CSipMutex * garrMutex = NULL;

// SSL 라이브러리를 multi-thread 에서 사용할 수 있기 위한 Lock/Unlock function
static void SSLLockingFunction( int mode, int n, const char * file, int line )
{
	if( mode & CRYPTO_LOCK )
	{
		garrMutex[n].acquire();
	}
	else
	{
		garrMutex[n].release();
	}
}

// SSL 라이브러리를 multi-thread 에서 사용할 수 있기 위한 ID function
static unsigned long SSLIdFunction( )
{
#ifdef WIN32
	return GetCurrentThreadId();
#else
	return (unsigned long)pthread_self();
#endif
}

// SSL 라이브러리를 multi-thread 기반으로 시작한다.
bool SSLStart( )
{
	if( garrMutex )
	{
		return true;
	}

	garrMutex = new CSipMutex[ CRYPTO_num_locks() ];
	if( garrMutex == NULL )
	{
		CLog::Print( LOG_ERROR, "%s CMutex[] new error", __FUNCTION__ );
		return false;
	}

	CRYPTO_set_id_callback( SSLIdFunction );
	CRYPTO_set_locking_callback( SSLLockingFunction );

	if( !SSL_library_init() )
	{
		CLog::Print( LOG_ERROR, "SSL_init_library error" );
		return false;
	}

	SSL_load_error_strings();
	SSLeay_add_ssl_algorithms();

	return true;
}

// SSL 라이브러리를 중지시킨다.
static bool SSLStop( )
{
	CRYPTO_set_id_callback(NULL);
	CRYPTO_set_locking_callback(NULL);

	if( garrMutex )
	{
		delete [] garrMutex;
		garrMutex = NULL;
	}

	return true;
}

static void SSLPrintError( )
{
	CLog::Print( ERR_print_errors_fp );
}

// SSL 서버 라이브러리를 시작한다.
bool SSLServerStart( const char * szCertFile, const char * szKeyFile, const char * szCaCertFile )
{
	int	n;

	if( szCertFile == NULL ) return false;
	if( IsExistFile( szCertFile ) == false )
	{
		CLog::Print( LOG_ERROR, "cert file(%s) is not found", szCertFile );
		return false;
	}

	// 키 파일 미지정이면 인증서 파일에서 읽는다(cert+key 결합 PEM). 별도 파일 지정 시 존재 확인.
	const char * pszKey = ( szKeyFile && szKeyFile[0] ) ? szKeyFile : szCertFile;
	if( IsExistFile( pszKey ) == false )
	{
		CLog::Print( LOG_ERROR, "key file(%s) is not found", pszKey );
		return false;
	}

	if( SSLStart() == false ) return false;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	gpsttServerMeth = TLS_server_method();
	gpsttClientMeth = TLS_client_method();
#else
	gpsttServerMeth = TLSv1_server_method();
	gpsttClientMeth = TLSv1_client_method();
#endif
	if( (gpsttServerCtx = SSL_CTX_new( gpsttServerMeth )) == NULL )
	{
		CLog::Print( LOG_ERROR, "SSL_CTX_new error - server" );
		return false;
	}

	if( (gpsttClientCtx = SSL_CTX_new( gpsttClientMeth )) == NULL )
	{
		CLog::Print( LOG_ERROR, "SSL_CTX_new error - client" );
		return false;
	}

	// 체인 파일 로딩 — PEM 의 첫 인증서를 서버 인증서로, 나머지를 중간 CA 체인으로 등록해
	//   핸드셰이크 Certificate 목록에 함께 실어 보낸다. use_certificate_file 은 첫 인증서만
	//   등록해 중간 CA 가 상대에게 전달되지 않았다(상대가 발급자를 찾지 못해 검증 실패).
	//   인증서 1장뿐인 PEM 에서는 동작이 동일하다.
	if( SSL_CTX_use_certificate_chain_file( gpsttServerCtx, szCertFile ) <= 0 )
	{
		CLog::Print( LOG_ERROR, "SSL_CTX_use_certificate_chain_file error" );
		SSLPrintError( );
		return false;
	}

	if( ( n = SSL_CTX_use_PrivateKey_file( gpsttServerCtx, pszKey, SSL_FILETYPE_PEM ) ) <= 0 )
	{
		CLog::Print( LOG_ERROR, "SSL_CTX_use_PrivateKey_file(%s) error(%d) — 키가 없는 인증서 파일이면 "
		                        "KeyFile 을 지정하거나 cert+key 결합 PEM 을 쓴다", pszKey, n );
		return false;
	}
	
	if( !SSL_CTX_check_private_key( gpsttServerCtx ) )
	{
		CLog::Print( LOG_ERROR, "[SSL] Private key does not match the certificate public key");
		return false;
	}

	if( szCaCertFile && strlen( szCaCertFile ) > 0 )
	{
		if( SSL_CTX_load_verify_locations( gpsttServerCtx, szCaCertFile, NULL ) == 0 )
		{
			CLog::Print( LOG_ERROR, "[SSL] CaCertFile(%s) load error", szCaCertFile );
			return false;
		}

		SSL_CTX_set_verify( gpsttServerCtx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL );
		//SSL_CTX_set_verify_depth( gpsttServerCtx, 1 );
	}

	gbStartSslServer = true;

	return true;
}

// SSL 서버 라이브러리를 종료한다.
bool SSLServerStop( )
{
	if( gbStartSslServer )
	{
		SSLStop();

		gclsServerCtxMutex.acquire();
		if( gpsttServerCtx )
		{
			SSL_CTX_free( gpsttServerCtx );
			gpsttServerCtx = NULL;
		}
		gclsServerCtxMutex.release();

		if( gpsttClientCtx )
		{
			SSL_CTX_free( gpsttClientCtx );
			gpsttClientCtx = NULL;
		}

		gbStartSslServer = false;
	}

	return true;
}

// SSL 클라이언트 라이브러리를 시작한다.
bool SSLClientStart( )
{
	if( SSLStart() == false ) return false;

	// 버전 유연 메서드를 쓴다 — TLSv1_client_method() 는 **TLS 1.0 전용**이라 최신 서버와
	//   handshake 가 성립하지 않는다(OpenSSL 3 기본 보안수준에서 거절). SSLServerStart 는
	//   이미 TLS_client_method() 를 쓰고 있어, 서버 겸용 프로세스에서만 우연히 정상이었다.
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	gpsttClientMeth = TLS_client_method();
#else
	gpsttClientMeth = TLSv1_client_method();
#endif
	if( (gpsttClientCtx = SSL_CTX_new( gpsttClientMeth )) == NULL )
	{
		CLog::Print( LOG_ERROR, "SSL_CTX_new error - client" );
		return false;
	}

	gbStartSslServer = true;

	return true;
}

// SSL 클라이언트 라이브러리를 종료한다.
bool SSLClientStop( )
{
	if( gbStartSslServer )
	{
		SSLStop();

		if( gpsttClientCtx )
		{
			SSL_CTX_free( gpsttClientCtx );
			gpsttClientCtx = NULL;
		}

		gbStartSslServer = false;
	}

	return true;
}

// 프로세스가 종료될 때에 최종적으로 실행하여서 openssl 메모리 누수를 출력하지 않는다.
void SSLFinal()
{
	SSLStop();

	ERR_free_strings();

#ifdef USE_TLS_FREE
	// http://clseto.mysinablog.com/index.php?op=ViewArticle&articleId=3304652
	ERR_remove_state(0);
	COMP_zlib_cleanup();
	OBJ_NAME_cleanup(-1);
	CRYPTO_cleanup_all_ex_data();
	EVP_cleanup();
	sk_SSL_COMP_free( SSL_COMP_get_compression_methods() );
#endif
}

// SSL 세션을 연결한다.
/** 클라이언트 SSL_CTX 지연 초기화 — HTTPS 클라이언트(CSP→CSC AV 조회 등)는 SIP TLS 리스너와
 *  무관하게 동작해야 한다. 종전엔 gpsttClientCtx 가 SSLServerStart(stack-global TLS 리스너 기동)나
 *  SSLClientStart(TLS 클라이언트 전용 모드)에서만 만들어져, per-listener 인증서로만 TLS 접속점을
 *  올린 노드(또는 TLS 접속점을 기동 후 hot-add 한 노드)에서는 ctx=NULL 로 SSLConnect 가 조용히
 *  실패했다(CIMS 실측: dev CSP 의 AKA AV 조회 504 — 08-27). 첫 사용 시점에 없으면 만든다.
 */
static CSipMutex gclsClientCtxMutex;

static bool SSLEnsureClientCtx( )
{
	if( gpsttClientCtx ) return true;
	gclsClientCtxMutex.acquire();
	bool bOk = ( gpsttClientCtx != NULL );
	if( bOk == false )
	{
		bOk = SSLClientStart();
		if( bOk )
		{
			CLog::Print( LOG_SYSTEM, "[SSL] client ctx lazily initialized (no TLS listener at start)" );
		}
		else
		{
			CLog::Print( LOG_ERROR, "[SSL] client ctx lazy init failed" );
		}
	}
	gclsClientCtxMutex.release();
	return bOk;
}

bool SSLConnect( Socket iFd, SSL ** ppsttSsl )
{
	SSL * psttSsl;

	if( SSLEnsureClientCtx() == false )
	{
		return false;
	}
	if( (psttSsl = SSL_new( gpsttClientCtx )) == NULL )
	{
		CLog::Print( LOG_ERROR, "[SSL] SSL_new(client ctx) error" );
		return false;
	}
	
	try
	{
		SSL_set_fd( psttSsl, (int)iFd );
		int iRet = SSL_connect( psttSsl );
		if( iRet != 1 )
		{
			CLog::Print( LOG_ERROR, "[SSL] SSL_connect error(ret=%d, err=%d)", iRet,
			             SSL_get_error( psttSsl, iRet ) );
			SSLPrintError( );
			SSL_free( psttSsl );
			return false;
		}
	}
	catch( ... )
	{
		CLog::Print( LOG_ERROR, "[SSL] SSLConnect() undefined error" );
		SSL_free( psttSsl );
		return false;
	}

	*ppsttSsl = psttSsl;

	return true;
}

// 클라이언트 SSL 접속 요청을 허용한다.
// ── R5.c: per-listener SSL_CTX helpers ─────────────────────────
SSL_CTX * SSLServerCtxCreate( const char * szCertFile, const char * szKeyFile, const char * szCaCertFile )
{
	if( szCertFile == NULL || szCertFile[0] == '\0' ) return NULL;
	if( IsExistFile( szCertFile ) == false )
	{
		CLog::Print( LOG_ERROR, "SSLServerCtxCreate: cert file '%s' not found", szCertFile );
		return NULL;
	}
	if( SSLStart() == false ) return NULL;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	const SSL_METHOD * pMeth = TLS_server_method();
#else
	SSL_METHOD * pMeth = TLSv1_server_method();
#endif
	SSL_CTX * ctx = SSL_CTX_new( pMeth );
	if( ctx == NULL )
	{
		CLog::Print( LOG_ERROR, "SSLServerCtxCreate: SSL_CTX_new error" );
		return NULL;
	}

	// 체인 파일 로딩 — 중간 CA 를 PEM 뒤에 이어붙이면 상대에게 함께 전달된다(위 SSLServerStart 주석 참조).
	if( SSL_CTX_use_certificate_chain_file( ctx, szCertFile ) <= 0 )
	{
		CLog::Print( LOG_ERROR, "SSLServerCtxCreate: use_certificate_chain_file('%s') error", szCertFile );
		SSLPrintError();
		SSL_CTX_free( ctx );
		return NULL;
	}

	const char * pszKey = ( szKeyFile && szKeyFile[0] ) ? szKeyFile : szCertFile;
	if( SSL_CTX_use_PrivateKey_file( ctx, pszKey, SSL_FILETYPE_PEM ) <= 0 )
	{
		CLog::Print( LOG_ERROR, "SSLServerCtxCreate: use_PrivateKey_file('%s') error", pszKey );
		SSLPrintError();
		SSL_CTX_free( ctx );
		return NULL;
	}

	if( !SSL_CTX_check_private_key( ctx ) )
	{
		CLog::Print( LOG_ERROR, "SSLServerCtxCreate: private key does not match certificate" );
		SSL_CTX_free( ctx );
		return NULL;
	}

	if( szCaCertFile && szCaCertFile[0] )
	{
		if( SSL_CTX_load_verify_locations( ctx, szCaCertFile, NULL ) == 0 )
		{
			CLog::Print( LOG_ERROR, "SSLServerCtxCreate: load_verify_locations('%s') error", szCaCertFile );
			SSL_CTX_free( ctx );
			return NULL;
		}
		SSL_CTX_set_verify( ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL );
	}

	return ctx;
}

void SSLServerCtxFree( SSL_CTX * ctx )
{
	if( ctx ) SSL_CTX_free( ctx );
}

SSL_CTX * SSLServerCtxAcquire( )
{
	SSL_CTX * psttCtx = NULL;

	gclsServerCtxMutex.acquire();
	psttCtx = gpsttServerCtx;
	if( psttCtx ) _SslCtxUpRef( psttCtx );
	gclsServerCtxMutex.release();

	return psttCtx;
}

bool SSLServerCtxReload( const char * szCertFile, const char * szKeyFile, const char * szCaCertFile )
{
	// 새 ctx 를 먼저 완성한다 — 파일 오타·권한 문제로 실패하면 기존 인증서를 그대로 유지해야 한다
	//   (교체 실패가 접속점 중단으로 번지지 않게).
	SSL_CTX * psttNew = SSLServerCtxCreate( szCertFile, szKeyFile, szCaCertFile );
	if( psttNew == NULL )
	{
		CLog::Print( LOG_ERROR, "SSLServerCtxReload: 새 ctx 생성 실패 — 기존 인증서 유지 (cert=%s)",
		             szCertFile ? szCertFile : "" );
		return false;
	}

	SSL_CTX * psttOld = NULL;
	gclsServerCtxMutex.acquire();
	psttOld = gpsttServerCtx;
	gpsttServerCtx = psttNew;
	gclsServerCtxMutex.release();

	// 우리 참조만 해제한다. 이미 맺어진 연결의 SSL 객체와 지금 핸드셰이크 중인 SSLServerCtxAcquire
	//   보유분이 각자 참조를 들고 있어, 실제 소멸은 마지막 사용자가 끝난 뒤다 → **무중단**.
	if( psttOld ) SSL_CTX_free( psttOld );

	CLog::Print( LOG_SYSTEM, "SSLServerCtxReload: TLS 인증서 교체 완료 — 기존 연결 유지, 새 핸드셰이크부터 적용 (cert=%s key=%s)",
	             szCertFile ? szCertFile : "", ( szKeyFile && szKeyFile[0] ) ? szKeyFile : "<same as cert>" );
	return true;
}

bool SSLServerIsStarted( )
{
	return gpsttServerCtx != NULL;
}

bool SSLAcceptWithCtx( Socket iFd, SSL_CTX * ctx, SSL ** ppsttSsl, bool bCheckClientCert, int iVerifyDepth, int iAcceptTimeout )
{
	// 전역 ctx 로 폴백하는 경우 **참조를 획득해서** 쓴다 — 무중단 교체(SSLServerCtxReload)가
	//   포인터를 바꾸고 옛 ctx 를 해제하는 사이에 SSL_new 가 dangling 을 잡는 것을 막는다.
	//   SSL_new 가 성공하면 그 SSL 이 자기 참조를 들고 있으므로 여기서 우리 참조는 놓는다.
	SSL_CTX * pUse = ctx;
	bool bOwnRef = false;
	if( pUse == NULL )
	{
		pUse = SSLServerCtxAcquire();
		bOwnRef = ( pUse != NULL );
	}
	SSL * psttSsl;

	if( pUse == NULL )
	{
		CLog::Print( LOG_ERROR, "SSLAcceptWithCtx: server ctx is null" );
		return false;
	}

	psttSsl = SSL_new( pUse );
	if( bOwnRef ) SSL_CTX_free( pUse );
	if( psttSsl == NULL )
	{
		CLog::Print( LOG_ERROR, "SSLAcceptWithCtx: SSL_new() error" );
		return false;
	}

	SSL_set_fd( psttSsl, (int)iFd );

	if( iAcceptTimeout > 0 )
	{
#ifdef WIN32
		int iTimeout = iAcceptTimeout;
		setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, (char *)&iTimeout, sizeof(iTimeout) );
#else
		struct timeval sttTime;
		sttTime.tv_sec = iAcceptTimeout / 1000;
		sttTime.tv_usec = ( iAcceptTimeout % 1000 ) * 1000;
		if( setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, &sttTime, sizeof(sttTime) ) == -1 )
		{
			CLog::Print( LOG_ERROR, "SSLAcceptWithCtx: SO_RCVTIMEO error(%d)", GetError() );
		}
#endif
	}

	try
	{
		if( bCheckClientCert )
		{
			SSL_set_verify( psttSsl, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT | SSL_VERIFY_CLIENT_ONCE, NULL );
			SSL_set_verify_depth( psttSsl, iVerifyDepth );
		}
		if( SSL_accept( psttSsl ) == -1 )
		{
			CLog::Print( LOG_ERROR, "SSLAcceptWithCtx: SSL_accept() error" );
			SSLPrintError();
			SSL_free( psttSsl );
			return false;
		}
	}
	catch( ... )
	{
		CLog::Print( LOG_ERROR, "[SSL] SSLAcceptWithCtx() undefined error" );
		SSL_free( psttSsl );
		return false;
	}

	if( iAcceptTimeout > 0 )
	{
#ifdef WIN32
		int iTimeout = 0;
		setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, (char *)&iTimeout, sizeof(iTimeout) );
#else
		struct timeval sttTime;
		sttTime.tv_sec = 0;
		sttTime.tv_usec = 0;
		if( setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, &sttTime, sizeof(sttTime) ) == -1 )
		{
			CLog::Print( LOG_ERROR, "SSLAcceptWithCtx: reset SO_RCVTIMEO error(%d)", GetError() );
		}
#endif
	}

	*ppsttSsl = psttSsl;
	return true;
}

bool SSLAccept( Socket iFd, SSL ** ppsttSsl, bool bCheckClientCert, int iVerifyDepth, int iAcceptTimeout )
{
	SSL * psttSsl;

	SSL_CTX * pCtx = SSLServerCtxAcquire();
	if( pCtx == NULL )
	{
		CLog::Print( LOG_ERROR, "SSLAccept: server ctx is null" );
		return false;
	}
	psttSsl = SSL_new( pCtx );
	SSL_CTX_free( pCtx );
	if( psttSsl == NULL )
	{
		CLog::Print( LOG_ERROR, "SSL_new() error" );
	  return false;
	}

	SSL_set_fd( psttSsl, (int)iFd );

	if( iAcceptTimeout > 0 )
	{
#ifdef WIN32
		int		iTimeout = iAcceptTimeout;
		setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, (char *)&iTimeout, sizeof(iTimeout) );
#else
		struct timeval	sttTime;

		sttTime.tv_sec = iAcceptTimeout / 1000;
		sttTime.tv_usec = ( iAcceptTimeout % 1000 ) * 1000;

		CLog::Print( LOG_DEBUG, "SO_RCVTIMEO(%d.%d)", sttTime.tv_sec, sttTime.tv_usec );
		if( setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, &sttTime, sizeof(sttTime) ) == -1 )
		{
			CLog::Print( LOG_ERROR, "setsockopt(SO_RCVTIMEO:%d.%d) error(%d)", sttTime.tv_sec, sttTime.tv_usec, GetError() );
		}
#endif
	}

	// QQQ : SSL 프로토콜이 아닌 경우에 메모리 에러가 발생하므로 아래와 같이
	//     : 막아 놓았음. 더 좋은 방법을 모색하여야 함.
	try
	{
		if( bCheckClientCert )
		{
			SSL_set_verify( psttSsl, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT | SSL_VERIFY_CLIENT_ONCE, NULL );
			SSL_set_verify_depth( psttSsl, iVerifyDepth );
		}

		if( SSL_accept( psttSsl ) == -1 )
		{
			CLog::Print( LOG_ERROR, "SSL_accept() error" );
			SSLPrintError( );
			SSL_free( psttSsl );
			return false;
		}
	}
	catch( ... )
	{
		CLog::Print( LOG_ERROR, "[SSL] SSL_accept() undefined error" );
		SSL_free( psttSsl );
		return false;
	}

	if( iAcceptTimeout > 0 )
	{
#ifdef WIN32
		int iTimeout = 0;	
		setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, (char *)&iTimeout, sizeof(iTimeout) );
#else
		struct timeval	sttTime;

		sttTime.tv_sec = 0;
		sttTime.tv_usec = 0;
		if( setsockopt( iFd, SOL_SOCKET, SO_RCVTIMEO, &sttTime, sizeof(sttTime) ) == -1 )
		{
			CLog::Print( LOG_ERROR, "setsockopt(SO_RCVTIMEO:%d.%d) error(%d)", sttTime.tv_sec, sttTime.tv_usec, GetError() );
		}
#endif
	}

	*ppsttSsl = psttSsl;

	return true;
}

// SSL 프로토콜로 패킷을 전송한다.
int SSLSend( SSL * ssl, const char * szBuf, int iBufLen )
{
	int		n;	
	int		iSendLen = 0;
	
	try
	{
		while( 1 )
		{
			n = SSL_write( ssl, szBuf + iSendLen, iBufLen - iSendLen );
			if( n <= 0 ) return -1;
		
			iSendLen += n;
			if( iSendLen == iBufLen ) break;	
		}
	}
	catch( ... )
	{
		CLog::Print( LOG_ERROR, "[SSL] SSLSend() undefined error" );
	}
	
	return iBufLen;
}

// SSL 프로토콜로 수신된 패킷을 읽는다.
int SSLRecv( SSL * ssl, char * szBuf, int iBufLen )
{
	return SSL_read( ssl, szBuf, iBufLen );
}

// SSL 세션을 종료한다.
bool SSLClose( SSL * ssl )
{
	if( ssl ) 
	{
		SSL_free( ssl );
	}

	return true;
}

#ifdef WIN32
// SSL 서버에서 사용되는 cipher list 를 로그로 출력한다.
void SSLPrintLogServerCipherList( )
{
	if( gpsttServerCtx == NULL )
	{
		CLog::Print( LOG_ERROR, "gpsttServerCtx is null" );
		return;
	}

	int iNum = sk_SSL_CIPHER_num( gpsttServerCtx->cipher_list );
	for( int i = 0; i < iNum; ++i )
	{
		const SSL_CIPHER *c = sk_SSL_CIPHER_value( gpsttServerCtx->cipher_list, i );
		CLog::Print( LOG_DEBUG, "[%s] [%s] [0x%04X] (%d)", SSL_CIPHER_get_version(c), SSL_CIPHER_get_name(c), c->id - 0x3000000, i );
	}
}

// SSL 클라이언트에서 사용되는 cipher list 를 로그로 출력한다.
void SSLPrintLogClientCipherList( )
{
	if( gpsttClientCtx == NULL )
	{
		CLog::Print( LOG_ERROR, "gpsttServerCtx is null" );
		return;
	}

	int iNum = sk_SSL_CIPHER_num( gpsttClientCtx->cipher_list );
	for( int i = 0; i < iNum; ++i )
	{
		const SSL_CIPHER *c = sk_SSL_CIPHER_value( gpsttClientCtx->cipher_list, i );
		CLog::Print( LOG_DEBUG, "[%s] [%s] [0x%04X] (%d)", SSL_CIPHER_get_version(c), SSL_CIPHER_get_name(c), c->id - 0x3000000, i );
	}
}
#endif

#endif
