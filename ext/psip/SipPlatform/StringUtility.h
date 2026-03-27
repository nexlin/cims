#ifndef _STRING_UTILITY_H_
#define _STRING_UTILITY_H_

#include "SipPlatformDefine.h"
#include "Log.h"
#include <string>
#include <list>
#include <vector>

typedef std::list< std::string > STRING_LIST;
typedef std::vector< std::string > STRING_VECTOR;

void ReplaceString( std::string & strCallId, const char * pszBefore, const char * pszAfter );

bool SearchValue( std::string & strText, const char * pszKey, char cSep, std::string & strValue );
bool SearchValue( std::string & strText, const char * pszKey, char cSep, int & iValue );

bool SearchStringList( STRING_LIST & clsList, const char * pszKey );
bool DeleteStringList( STRING_LIST & clsList, const char * pszKey );
void InsertStringList( STRING_LIST & clsList, const char * pszKey );
void InsertStringList( STRING_LIST & clsList, STRING_LIST & clsSrcList );
void LogStringList( EnumLogLevel eLevel, const char * pszName, STRING_LIST & clsList );

void LeftTrimString( std::string & strText );
void RightTrimString( std::string & strText );
void TrimString( std::string & strText );
void SplitString( const char * pszText, STRING_LIST & clsList, char cSep );
void SplitString( const char * pszText, STRING_VECTOR & clsList, char cSep );

uint32_t GetUInt32( const char * pszText );
uint64_t GetUInt64( const char * pszText );
int GetInt( const char * pszText, int iTextLen );

bool HexToString( const char * pszInput, std::string & strOutput );
void StringToHex( const char * pszInput, int iInputLen, std::string & strOutput );
bool IsPrintString( const char * pszText, int iTextLen );
void DeQuoteString( std::string & strInput, std::string & strOutput );

#endif
