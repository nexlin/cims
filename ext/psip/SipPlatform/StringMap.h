#ifndef _STRING_MAP_H_
#define _STRING_MAP_H_

#include <string>
#include <map>
#include "SipMutex.h"

typedef std::map< std::string, std::string > STRING_MAP;


class CStringMap
{
public:
	CStringMap();
	~CStringMap();

	bool Insert( const char * pszKey, const char * pszValue );
	bool Select( const char * pszKey );
	bool Select( const char * pszKey, std::string & strValue );
	bool Delete( const char * pszKey );
	int GetCount( );
	void DeleteAll( );

private:
	CSipMutex	m_clsMutex;
	STRING_MAP	m_clsMap;
};

#endif
