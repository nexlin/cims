#ifndef __PHANDLER_H__
#define __PHANDLER_H__

#include <list>
//#include "pbase.h"
#include <memory>
#include <memory>

#include "pevent.h"
#include "paio.h"

class PHandler
{
	public:
		PHandler(const std::string name) : _name(name){ };
		virtual ~PHandler() {};
		virtual bool init() { return true; } 
		virtual bool proc() = 0;  //{ return false; }; //if need, always 
		virtual bool proc(int id, const std::string & name, PEvent::Ptr spEvent) = 0; //is event
		std::string & name() { return _name; }
		std::string   info();
		bool run();
		bool addAio(PAio * spAio);
		bool delAio(PAio * spAio);
		bool delAio_lockFree(PAio * spAio); // temporay
		bool clearAio();
		unsigned int  aioSize() { return _ilist.size(); }
		void delAioUnlock(PAio * spAio) { _ilist.remove(spAio); }
	protected:
	private:
		std::list<PAio*>       _ilist;
		std::string                _name;
		PMutex                     _mutex;
};

#endif //__PHANDLER_H__
