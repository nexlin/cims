#ifndef __PMSGEVENT_H__
#define __PMSGEVENT_H__

#include "pevent.h"

class PMsgEvent : public PEvent
{
public:
   PMsgEvent(const std::string & name): PEvent(name), _pMsg(NULL), _msgLen(0) { }
   virtual ~PMsgEvent() {
      if (_pMsg) { 
         delete [] _pMsg;
      }
   };

   virtual const char* getMsg() const {
      return _pMsg; 
   }

   virtual int getMsgLen() const {
      return _msgLen;
   }

   void setMsg (const char* pMsg, unsigned int uSize) { 
      _pMsg = new char[uSize];            
      memcpy(_pMsg, pMsg, uSize);            
      _msgLen = uSize;
   }

   void addMsg (const char* pMsg, unsigned int uSize) { 
      char * pTmpMsg = new char[_msgLen+uSize];     
      if(_msgLen > 0) {
         memcpy(pTmpMsg, _pMsg, _msgLen);            
         delete [] _pMsg;
      }       
      memcpy(pTmpMsg+_msgLen, pMsg, uSize);


      _pMsg = pTmpMsg;
      _msgLen += uSize;
   }

protected:
   char* _pMsg;
   unsigned int _msgLen;

};

#endif
