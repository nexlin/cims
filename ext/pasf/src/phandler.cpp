#include "pbase.h"
#include "pevent.h"
#include "paio.h"
#include "phandler.h"
#include <typeinfo>

bool PHandler::addAio(PAio * spAio) 
{
	_mutex.lock();
	
	_ilist.push_back(spAio);
	_mutex.unlock();
	return true;
};
bool PHandler::delAio(PAio * spAio) 
{
	_mutex.lock();
	_ilist.remove(spAio);
	_mutex.unlock();
	return true;
}
bool PHandler::delAio_lockFree(PAio * spAio) 
{
    _ilist.remove(spAio);
    return true;
}
bool PHandler::clearAio() 
{
	_mutex.lock();
	_ilist.clear();
	_mutex.unlock();
	return true;
}
bool PHandler::run() 
{
	std::list<PAio*>::iterator it;
	PEvent::Ptr pevent;
	bool bWorking = false;
	_mutex.lock();
	bWorking = proc();

	for(it = _ilist.begin(); it!=_ilist.end(); ++it) 
	{
		bool bRet = (*it)->read(pevent);
		if(bRet) 
		{
			bWorking = true;
			proc((*it)->id(), (*it)->name(), pevent);
		}
	}
	_mutex.unlock();
	return bWorking;
}
std::string PHandler::info() 
{
	std::stringstream ss;
	ss << "   - ";
	ss << "PHandler(" << _name << ") :" << std::endl;
	std::list<PAio *>::iterator it;
	_mutex.lock();
	for(it = _ilist.begin(); it!=_ilist.end(); ++it) 
	{
		ss << "     - ";
		ss << (*it)->info();
		ss << std::endl;
	}
	_mutex.unlock();
	return ss.str();
}

