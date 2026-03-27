#include "StringMap.h"
#include "MemoryDebug.h"

CStringMap::CStringMap()
{
}

CStringMap::~CStringMap()
{
}

bool CStringMap::Insert( const char * pszKey, const char * pszValue )
{
	STRING_MAP::iterator	itMap;
	bool	bRes = false;

	if( pszKey == NULL ) return false;
	if( pszValue == NULL ) pszValue = "";

	m_clsMutex.acquire();
	itMap = m_clsMap.find( pszKey );
	if( itMap == m_clsMap.end() )
	{
		m_clsMap.insert( STRING_MAP::value_type( pszKey, pszValue ) );
		bRes = true;
	}
	m_clsMutex.release();

	return bRes;
}

/**
 * @ingroup SipPlatform
 * @brief 문자열 맵 자료구조에 문자열 키가 존재하는지 검색한다.
 * @param pszKey	문자열 키
 * @returns 문자열 맵 자료구조에 문자열 키가 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CStringMap::Select( const char * pszKey )
{
	STRING_MAP::iterator	itMap;
	bool	bRes = false;

	if( pszKey == NULL ) return false;

	m_clsMutex.acquire();
	itMap = m_clsMap.find( pszKey );
	if( itMap != m_clsMap.end() )
	{
		bRes = true;
	}
	m_clsMutex.release();

	return bRes;
}

/**
 * @ingroup SipPlatform
 * @brief 문자열 맵 자료구조에 문자열 키가 존재하는지 검색한다.
 * @param pszKey		문자열 키
 * @param strValue	키에 대한 값을 저장할 변수
 * @returns 문자열 맵 자료구조에 문자열 키가 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CStringMap::Select( const char * pszKey, std::string & strValue )
{
	STRING_MAP::iterator	itMap;
	bool	bRes = false;

	strValue.clear();

	if( pszKey == NULL ) return false;

	m_clsMutex.acquire();
	itMap = m_clsMap.find( pszKey );
	if( itMap != m_clsMap.end() )
	{
		strValue = itMap->second;
		bRes = true;
	}
	m_clsMutex.release();

	return bRes;
}

/**
 * @ingroup SipPlatform
 * @brief 문자열 맵 자료구조에 문자열 키가 존재하면 삭제한다.
 * @param pszKey	문자열 키
 * @returns 문자열 맵 자료구조에 문자열 키가 존재하면 true 를 리턴하고 그렇지 않으면 false 를 리턴한다.
 */
bool CStringMap::Delete( const char * pszKey )
{
	STRING_MAP::iterator	itMap;
	bool	bRes = false;

	if( pszKey == NULL ) return false;

	m_clsMutex.acquire();
	itMap = m_clsMap.find( pszKey );
	if( itMap != m_clsMap.end() )
	{
		m_clsMap.erase( itMap );
		bRes = true;
	}
	m_clsMutex.release();

	return bRes;
}

/**
 * @ingroup SipPlatform
 * @brief 문자열 맵 자료구조에 저장된 문자열 개수를 리턴한다.
 * @returns 문자열 맵 자료구조에 저장된 문자열 개수를 리턴한다.
 */
int CStringMap::GetCount( )
{
	int iCount;

	m_clsMutex.acquire();
	iCount = (int)m_clsMap.size();
	m_clsMutex.release();

	return iCount;
}

/**
 * @ingroup SipPlatform
 * @brief 자료구조에 저장된 데이터를 모두 삭제한다.
 */
void CStringMap::DeleteAll( )
{
	m_clsMutex.acquire();
	m_clsMap.clear();
	m_clsMutex.release();
}
