#ifndef __SVCEVENT_H__
#define __SVCEVENT_H__

#include "pcommhdr.h" 


class CSvcEvent : public PEvent
{
public:
   CSvcEvent(const std::string & name): PEvent(name), _pSvcMsg(NULL) { }
   virtual ~CSvcEvent() {
        if (_pSvcMsg) { 
            delete [] _pSvcMsg;
        }
   };

   virtual const char* getMsg() const {
      return _pSvcMsg; 
   }

   virtual int getMsgLen() const {
       if (_pSvcMsg) {
           return ((SVCComMsgHdr*)_pSvcMsg)->uiMsgLen;
       }

       return 0;
   }

   void setMsg (const char* pSvcMsg, unsigned int uSize) { 
        _pSvcMsg = new char[uSize];            
        memcpy(_pSvcMsg, pSvcMsg, uSize);            
   }

protected:
   char* _pSvcMsg;

};

#endif
