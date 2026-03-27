#ifndef __PUDPAIO_H__
#define __PUDPAIO_H__

#include "paio.h"
#include "pmsgevent.h"
#include "psocket.h"

using namespace std;

class PUdpEvent : public PMsgEvent
{
public:
   enum {
      MTU_SIZE = 1500,
   };

   PUdpEvent(const std::string & name): PMsgEvent(name), _portRmt(0)
   { }

   virtual ~PUdpEvent() {}

   void setRmt(const std::string ip, unsigned int port) 
   { 
	  _ipRmt = ip;
      _portRmt = port;
   }
   const std::string &  getIpRmt() { return _ipRmt; } 
   unsigned int getPortRmt() { return _portRmt; }

   unsigned int _portRmt;
   std::string  _ipRmt;
};

class PUdpAio : public PAio
{
public:
    PUdpAio(int id, const std::string &name);
    virtual ~PUdpAio(); 

    bool open(const std::string& addr, const unsigned int port); 
    // 로컬 loc, port, rmt 확인 필요
    //bool open(const std::string& ipLoc, const unsigned int portLoc, const std::string& ipRmt, const unsigned int portRmt);

    void close();

    bool read(PEvent::Ptr & pevent); 
    bool write(PEvent::Ptr pevent);

protected:
    PUdpSocket * _spUdpSock;

    string _strAddress;
    unsigned int _nPort;

};

#endif // __PUDPAIO__
