#ifndef __PIPCAIO_H__
#define __PIPCAIO_H__

#include <memory>
#include <memory>

#include "paio.h"



class PIPCAio : public PAio
{
public:
   PIPCAio(int id, const std::string name);
   virtual ~PIPCAio(); 

   bool open(const std::string srcName, int srcQueLen,
             const std::string dstName, int dstQueLen);

   bool read(PEvent::Ptr & spEvent);
   bool write(PEvent::Ptr spEvent);

   bool close();

   virtual const std::string info();
private: 
   void *         _pctx;

   PMutex         _mutex;
};

#endif //__PIPCAIO_H__
