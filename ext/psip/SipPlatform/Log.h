#ifndef _LOG_H_
#define _LOG_H_

#include "SipPlatformDefine.h"
#include <stdio.h>
#include "SipMutex.h"
#include "Directory.h"

#define LOG_MAX_SIZE								1024*8
#define FULLPATH_FILENAME_MAX_SIZE	1024
#define MIN_LOG_FILE_SIZE						1024*1024
#define MAX_LOG_FILE_SIZE						1024*1024*1024
#define DEFAULT_LOG_FILE_SIZE				1024*1024*10
#define DEFAULT_LOG_FOLDER_SIZE			3145728000UL

/** 
 * @ingroup SipPlatform
 * @brief 로그 레벨 관련 ENUM
 *	CLog 에서 사용하는 ENUM 이다.
 */
enum EnumLogLevel
{
	/** 에러 로그 */
	LOG_ERROR = 0x0001,
	/** 정보 로그 */
	LOG_INFO  = 0x0010,
	/** 디버그 로그 */
	LOG_DEBUG = 0x0100,
	/** 네트워크 데이타 디버그 로그 */
	LOG_NETWORK = 0x0200,
	/** 시스템 로그 */
	LOG_SYSTEM = 0x400,
	/** SQL 로그 */
	LOG_SQL = 0x800
};

/**
 * @ingroup SipPlatform
 * @brief 로그 메시지를 프로그램에서 처리하고 싶은 경우에 사용한다.
 */
class ILogCallBack
{
public:
	virtual ~ILogCallBack(){};

	virtual void Print( EnumLogLevel eLevel, const char * fmt, ... ) = 0;
};

/** 
 * @ingroup SipPlatform
 * @brief 로그 관련 클래스
 */
class CLog
{
private:
	static char				* m_pszDirName;		// 로그 파일을 저장할 디렉토리 이름
	static char				m_szDate[9];		// 현재 저장중인 로그 파일 이름
	static char				m_szPrefix[64];		// 로그 파일 이름 prefix (프로세스 이름)
	static FILE				* m_sttFd;			// 로그 파일 지정자

	static CSipMutex	* m_pThreadMutex;	// Thread Mutex

	static int				m_iLevel;				// 로그 파일에 저장할 레벨
	static int				m_iMaxLogSize;	// 하나의 로그 파일에 저장할 수 있는 최대 크기
	static int				m_iLogSize;			// 현재까지 저장된 로그 크기
	static int				m_iIndex;				// 로그 파일 인덱스
	static int64_t		m_iMaxFolderSize;	// 로그 폴더의 최대 크기
	static int64_t		m_iFolderSize;		// 로그 폴더 크기

	static ILogCallBack * m_pclsCallBack;	// 로그 callback

public:
	static bool SetDirectory( const char * pszDirName );
	static void SetPrefix( const char * pszPrefix );
	static void Release();

	static void SetCallBack( ILogCallBack * pclsCallBack );

	static int Print( EnumLogLevel iLevel, const char * fmt, ... );
	static void Print( void (* func)( FILE * fd ) );

	static int GetLevel( );
	static void SetLevel( int iLevel );
	static void SetNullLevel();

	static void SetDebugLevel( );
	static bool IsPrintLogLevel( EnumLogLevel iLevel );
	static void SetMaxLogSize( int iSize );
	static void SetMaxFolderSize( int64_t iSize );
	static int GetLogIndex();
	static void DeleteOldFile( );

	static void SortFileList( FILE_LIST & clsFileList );
	static void PrintCallStack( EnumLogLevel iLevel );
};

#endif
