#ifndef _MONITOR_STRING_H_
#define _MONITOR_STRING_H_

#include "SipPlatformDefine.h"
#include <string>

// 자료구조 모니터링
#define MR_COL_SEP							"|"
#define MR_ROW_SEP							"\n"


/**
 * @ingroup SipStack
 * @brief 자료구조 모니터링을 위한 문자열 저장 및 생성 클래스
 */
class CMonitorString
{
public:
	CMonitorString();
	~CMonitorString();

	void AddCol( const char * pszValue );
	void AddCol( const std::string & strValue );
	void AddCol( const std::string & strIp, int iPort );
	void AddCol( uint32_t iIp, uint16_t iPort );
	void AddCol( int iValue );
	void AddCol( uint32_t iValue );
	void AddCol( time_t iTime );
	void AddCol( bool bValue );

	void AddRow( const char * pszValue );
	void AddRow( const std::string & strValue );
	void AddRow( const std::string & strIp, int iPort );
	void AddRow( uint32_t iIp, uint16_t iPort );
	void AddRow( int iValue );
	void AddRow( uint32_t iValue );
	void AddRow( time_t iTime );
	void AddRow( bool bValue );

	void Clear( );

	const char * GetString();
	int GetLength();

private:
	std::string m_strBuf;
};

#endif
