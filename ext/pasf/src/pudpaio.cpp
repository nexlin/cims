#include "pudpaio.h"

PUdpAio::PUdpAio(int id, const std::string &name) : PAio(id, name)
{
   _spUdpSock = NULL;
}

PUdpAio::~PUdpAio()
{
   close();
}


bool PUdpAio::write(PEvent::Ptr pevent)
{
   // send
   if (!_spUdpSock) {
       PDPRINT("PUdpAio::write UdpSocket is null\n");
       return false;
   }

   PUdpEvent * spUdpEvent = dynamic_cast<PUdpEvent*>(pevent.get());
   if (!spUdpEvent) {
       //PDPRINT("PUdpAio::write event is not PUpdEvent\n");
       // delete spEvent;
       return false;
   }

   char * ipRmt = (char *)spUdpEvent->getIpRmt().c_str();
   unsigned int portRmt = spUdpEvent->getPortRmt();

   int nSendLen = _spUdpSock->sendTo(spUdpEvent->getMsg(), spUdpEvent->getMsgLen(),
                                   portRmt, ipRmt);
   if (nSendLen <= 0) {
      PDPRINT("PUdpAio::write nSendLen=%d\n", nSendLen);
      return false;
   }

   // delete pevent; // managed by shared_ptr
   return true;
}

bool PUdpAio::read(PEvent::Ptr & spEvent)
{

   char data[PUdpEvent::MTU_SIZE];

   char ipRmt[16];
   unsigned int portRmt;

   // if sock Invalid
   int nRecvLen = _spUdpSock->recvFrom(data, PUdpEvent::MTU_SIZE, ipRmt, portRmt);
   if (nRecvLen < 0) {
      return false;
   }

   PUdpEvent *  spMsg = new PUdpEvent(name() + "->read");
   spMsg->setRmt(ipRmt, portRmt);
   spMsg->setMsg(data, nRecvLen);

   spEvent.reset(spMsg);

   return true;
}

bool PUdpAio::open(const std::string& rStrAddr, const unsigned int nPort)
{
   _spUdpSock = new PUdpSocket;
   _spUdpSock->open(rStrAddr, nPort);
   _spUdpSock->setNonBlock();

   _strAddress = rStrAddr;
   _nPort = nPort;
   return true;
}


void PUdpAio::close()
{
   if (_spUdpSock) {
      _spUdpSock->close();
          delete _spUdpSock;
          _spUdpSock = NULL;
   }
}
