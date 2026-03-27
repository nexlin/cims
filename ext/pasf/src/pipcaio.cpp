#include <map>
#include <queue>
#include <mutex>

#include "pipcaio.h"


// Internal Async I/O  Addr (Queue)
class PIPCPoint 
{
public:
   PIPCPoint(const std::string & name, int qlen) 
            : _name(name){ }

   ~PIPCPoint() { }

   bool put(PEvent::Ptr spEvent) {
      std::lock_guard<std::mutex> lock(_qMutex);
      _que.push(spEvent);
      return true ;
   };

   bool get(PEvent::Ptr &spEvent) {
      std::lock_guard<std::mutex> lock(_qMutex);
      if (_que.empty()) return false;
      spEvent = _que.front();
      _que.pop();
      return true;
   };

   void clear() {
      std::lock_guard<std::mutex> lock(_qMutex);
      while(!_que.empty()) _que.pop();
   };

   const std::string & name() { return _name; }

private:
   std::string          _name;
   int                  _qlen;
   //PQueue<PEvent *>		_que;
   std::queue<PEvent::Ptr> _que;
   std::mutex           _qMutex;
};

class PIPCPointAllocator
{
public:
   static PIPCPoint * create(const std::string name, int qlen) {
      PIPCPoint * spPoint;  
      std::map<std::string, PIPCPoint *>::iterator it;

      _mutex.lock();
      it = _amap.find(name);
      if(it == _amap.end()) {
         spPoint = new PIPCPoint(name, qlen);
         _amap[name] = spPoint;
      } else {
         spPoint = it->second;
      }
      _mutex.unlock();

      return spPoint;
   }
      
   static bool destroy(const std::string name) {
      std::map<std::string, PIPCPoint*>::iterator it;

      bool bRes = false;

      _mutex.lock();
      it = _amap.find(name);
      if(it != _amap.end()) {
         _amap.erase(it);
         bRes = true;
      }
      _mutex.unlock();
   
      return bRes;
   }

private: 
   static PMutex                                _mutex;
   static std::map<std::string, PIPCPoint*> _amap;
};

PMutex                                PIPCPointAllocator::_mutex;
std::map<std::string, PIPCPoint*> PIPCPointAllocator::_amap;


class PIPCContext 
{
public:
   PIPCPoint* spSrcPoint;
   PIPCPoint*  spDstPoint;
   
};

#define PCTX   ((PIPCContext *)_pctx)

PIPCAio::PIPCAio(int id, const std::string name):PAio(id, name) 
{
   _pctx = (void *)new PIPCContext();
}

PIPCAio::~PIPCAio() 
{
   close();
   delete PCTX;   
}

const std::string PIPCAio::info() 
{
   _mutex.lock();
   std::stringstream ss;

   ss << "PIPCAio(" << name() << "):" 
      << PCTX->spSrcPoint->name() 
      << "<->" 
      << PCTX->spDstPoint->name(); 
   
   _mutex.unlock();
   return ss.str();
}

bool PIPCAio::open(const std::string srcName, int srcQueLen,
                   const std::string dstName, int dstQueLen) 
{
   PAutoLock lock(_mutex);
   PCTX->spSrcPoint = PIPCPointAllocator::create(srcName, srcQueLen);
   if(!PCTX->spSrcPoint) return false;
   else PCTX->spSrcPoint->clear();
   PCTX->spDstPoint = PIPCPointAllocator::create(dstName, dstQueLen);
   if(!PCTX->spDstPoint) return false;

   return true;
}

bool PIPCAio::close()
{
   //destroy only local
   PAutoLock lock(_mutex);
   PIPCPoint * spPoint;
    
   spPoint = PCTX->spSrcPoint;
   if(!spPoint) return false;

   PIPCPointAllocator::destroy(spPoint->name());

   PCTX->spSrcPoint = NULL; 
   PCTX->spDstPoint = NULL; 

   return true;
}

bool PIPCAio::read(PEvent::Ptr & spEvent)
{
   PAutoLock lock(_mutex);
   if(!PCTX->spSrcPoint || !PCTX->spDstPoint) {
      // not opned or failed open 
      return false;//
   }
   return PCTX->spSrcPoint->get(spEvent); 
}

bool PIPCAio::write(PEvent::Ptr spEvent)
{
   PAutoLock lock(_mutex);
   if(!PCTX->spSrcPoint || !PCTX->spDstPoint) {
      PEPRINT("Error in PIPCAio::write()!\n");
      // not opned or failed open 
      return false;//
   }
   return PCTX->spDstPoint->put(spEvent);
}
