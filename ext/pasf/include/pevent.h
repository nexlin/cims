#ifndef __PEVENT_H__
#define __PEVENT_H__

#include <string>
#include <iostream>
#include <sstream>
#include <string.h>


#include <memory>
#include <memory>


class PEvent
{
public:
    enum PEVENT_TYPE
    {
        DEFAULT_TYPE    = 0,
        SOCK_EVENT_TYPE = 1,
    };

    enum PEVENT_SOCK_SUBTYPE
    {
        DEFAULT_SUBTYPE = 0,
        CONN_SUBTYPE    = 1,
        DISCONN_SUBTYPE = 2,
        ACCEPT_SUBTYPE  = 3,
    };

public:
   using Ptr = std::shared_ptr<PEvent>;

   PEvent(const std::string & name) : _name(name), _nEventType(DEFAULT_TYPE), _nEventSubType(DEFAULT_SUBTYPE) { 
      //std::cout << "# + Create PEvent :" 
      //          << _name 
      //          << "[" << this << "]" 
      //          << std::endl;
   }

   virtual ~PEvent() {
      //std::cout << "# - Delete PEvent :" 
      //          << _name 
      //          << "[" << this << "]" 
      //          << std::endl;
   }

#if 1
   virtual const char * getMsg() const  = 0 ;
   virtual int getMsgLen() const = 0 ;
#else // for next
   int getTypeId() const = 0; //classfication for swiching Event
   std::string getTypeName() const = 0;

   virtual bool addParam(const std::string & keys, const std::string & val) = 0;
   virtual bool getParam(const std::string & keys, std::string & val) const = 0;

   virtual const char * getData() const  = 0 ;
   virtual int getDataLen() const = 0 ;
#endif
   std::string & name() { return _name; }

   unsigned int getEventType() { return _nEventType; }
   unsigned int getEventSubType() { return _nEventSubType; }
   void setEventType(const unsigned int nEventType) { _nEventType=nEventType; }
   void setEventSubType(const unsigned int nEventSubType) { _nEventSubType=nEventSubType; }
   void getEventType(unsigned int &nEventType, unsigned int &nEventSubType) {
       nEventType = _nEventType; nEventSubType = _nEventSubType;
   }
   void setEventType(const unsigned nEventType, const unsigned int nEventSubType) {
       _nEventType = nEventType; _nEventSubType = nEventSubType;
   }


private:
   std::string _name;
   unsigned int _nEventType;
   unsigned int _nEventSubType;
};

class PJsonEvent : public PEvent
{
public:
   using Ptr = std::shared_ptr<PJsonEvent>;

   PJsonEvent(const std::string & name);
   virtual ~PJsonEvent();

   virtual const char * getMsg() const {
      return getString().data(); // make string  & return 
   }

   virtual int getMsgLen() const {
      return getString().length(); // make string & return
   }

   // set or add 
   bool setString(const std::string & strJson);
   const std::string &  getString() const;

   bool addParam(const std::string & keys, const std::string & val);

   bool getParam(const std::string & keys, std::string & val) const;

protected:
   void * _pctx;
};


// play_file 
/*
   msgtype
   userid
   session info,
    - board
    - trunk 
    - ch

   file info,
    - format
    - name
    - codec info
      - name
    - play time
 ==> { "msgtype":"PLAY_START_REQ", 
       "userid" :"XXXXXXXXXX",
       "ses" : { "board" : 1, "trunk" : 1, "ch" : 1},
       "file" : { "format":"3gp", "name" : "xxxx.3gp", 
                "codec" : { "name" : "AMR" },
                "playtime" : "1sec" }
     }

     string value; 
     PJsonEvent oldEvent;
     PJsonEvent event;

     oldEvent.getData("msgtype", value);
     event.setData("msgtype", value); 

     event.setData("userid", "XXXXXXXXXX");

     event.setData("ses.board", "1");
     event.setData("ses.trunk", "1");
     event.setData("ses.ch", "1");
     
     event.setData("file.format", "3gp");
     event.setData("file.format", "3gp");
     event.setData("file.codec.name", "AMR");

     bool bEvent = event.getData("file.codec.id", value); 

*/

#endif // __PEVENT_H__
