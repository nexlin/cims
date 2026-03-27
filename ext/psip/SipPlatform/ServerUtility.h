#ifndef _SERVER_UTILITY_H_
#define _SERVER_UTILITY_H_

#ifdef WIN32
#include <windows.h>
#endif

#include <vector>
#include <string>

void SetCoreDumpEnable();
bool Fork( bool bIsFork );
bool ChangeExecuteUser( const char * pszUserId );

#ifdef WIN32
bool StartThread( const char * pszName, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter );
#else
bool StartThread( const char * pszName, void *(*lpStartAddress)(void*), void * lpParameter );
#endif

#endif
