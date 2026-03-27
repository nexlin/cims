#include <iostream>
#include <unistd.h>
#include "pevent.h"
#include "paio.h"
#include "phandler.h"
#include "pmodule.h"
#include "psocket.h"
#include "pudphandler.h"

PUdpHandler::PUdpHandler(const std::string & name, int nSockAioId) 
   : PHandler(name), _bEnable(false), _strAddr(""), _nPort(0), _nSockAioId(nSockAioId)
{
   char szAioName[32];
   sprintf(szAioName, "%s-%d", "PUdpAio", _nSockAioId);

   PUdpAio *  spUdpAio = new PUdpAio(_nSockAioId, szAioName); 
   _spUdpAio = spUdpAio;
   addAio(_spUdpAio);
}

bool PUdpHandler::setPeer(bool bEnable, 
                             const std::string& strAddr, const int nPort)
{
   PAutoLock lock(_mutex);

   //if(_bEnable != bEnable || _nPort != nPort || _strAddr != strAddr) 
   {
      
      PDPRINT("# %s->setPeer: ip=%s/%s,  port=%d/%d\n", 
               name().c_str(), strAddr.c_str(), _strAddr.c_str(), nPort, _nPort);

      _bEnable = bEnable;
      _nPort = nPort;
      _strAddr = strAddr;

      _spUdpAio->close();

      _spUdpAio->open(strAddr, nPort);
   }
   return true;
}


