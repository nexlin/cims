#ifndef _MONITOR_H_
#define _MONITOR_H_

#include "MonitorCallBack.h"

class CMonitor : public IMonitorCallBack {
public:
    CMonitor();
    ~CMonitor();

    virtual bool RecvRequest( const char *pszRequest, CMonitorString &strResponse );

    virtual bool IsMonitorIp( const char *pszIp );
};

extern CMonitor gclsMonitor;

#endif
