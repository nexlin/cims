#ifndef _SERVER_SERVICE_PRIVATE_H_
#define _SERVER_SERVICE_PRIVATE_H_

#include "ServerService.h"

bool InstallService( );
bool UninstallService( );
void ServiceStart();
void LastMethod( int sig );

extern CServerService gclsService;
extern ServerFunc gpServerFunc;

#endif
