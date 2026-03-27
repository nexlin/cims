#ifndef __PAIO_H__
#define __PAIO_H__

#include <memory>
#include <memory>

#include "pbase.h"
#include "pevent.h"

// Async I/O
class PAio
{
public:
   PAio(int id, const std::string & name) : _id(id), _name(name) {};
   virtual ~PAio() {};

   virtual bool read(PEvent::Ptr & pevent) = 0;
   virtual bool write(PEvent::Ptr pevent) = 0;

   const int & id() { return _id; }
   const std::string & name() { return _name; }

   virtual const std::string info() { 
      std::stringstream info;
      info << "PAio("<< name() << ")";
      return info.str(); 
   }
private:
   int         _id;
   std::string _name;
};


#endif // __PAIO_H__
