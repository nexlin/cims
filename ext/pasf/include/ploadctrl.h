#ifndef __PLOAD_CONTROL_H__
#define __PLOAD_CONTROL_H__


#include "pbase.h"

//template default. get cps



enum DEFAULT_VALUE {
   E_MAX_TIMEINTERVAL   =  1000, //every 1sec
   E_MAX_PERIOD_BLOCK    =  20,  //if block = 10 & interval = 1000, 1block size is 100msec(1000/10)
};

//template<unsigned long TIMEINTERVAL = E_MAX_TIMEINTERVAL,
 //     int PERIOD_BLOCK = E_MAX_PERIOD_BLOCK>
class CLoadCalculator
{
public:
   CLoadCalculator(unsigned long interval=E_MAX_TIMEINTERVAL, int block_count=E_MAX_PERIOD_BLOCK)
   {
      _ulInterval = interval;
      _unPeriodBlockCount = block_count > E_MAX_PERIOD_BLOCK ? E_MAX_PERIOD_BLOCK : block_count; // MAX:20
      _unPeriodBlockCount = _unPeriodBlockCount < 2 ? 2 : _unPeriodBlockCount; //MIN : 2
      
      _ulPeriodInterval   = _ulInterval / _unPeriodBlockCount;
      
      _ulPrevPeriodIndex  = 0;

      _unSumIn            = 0;
      _unIntervalSumIn    = 0;
      _unHeadPeriodSumIn  = 0;

      memset(_unarrPeriodSumIn, 0, sizeof(unsigned int) * block_count);

      // *** current load with input load
      _unCurLoadIn       = 0;
      _unPeakLoadIn      = 0;

      _unSumOut           = 0;
      _unIntervalSumOut   = 0;
      _unHeadPeriodSumOut = 0;
      memset(_unarrPeriodSumOut, 0, sizeof(unsigned int) * block_count);

      // *** current load without input load
      _unCurLoadOut       = 0;
      _unPeakLoadOut      = 0;
      //_ulStartTime        = getTimeMS(); //current time
   }

   ~CLoadCalculator(){ }

   //whether is under the max csp that user defined.
   bool calcLoad(unsigned int load, unsigned int& rcps ,unsigned int max_load) 
   {
      unsigned long ulPeriodIndex;
      unsigned int unCurrentIndex;
      unsigned int unRemained;
      unsigned int unRemainedRate; 
      unsigned int unDiff;
      unsigned long ulCurTime;
      unsigned int unTmpLoadOut;
      
      //unsigned int unLoadBits = load<<3;
      unsigned int unLoadBits = load;

      ulCurTime      = _currentTime();

      PAutoLock lock(_mutex);
      //_pLock->lock(); //////////////
      ulPeriodIndex  = ulCurTime / _ulPeriodInterval; 
      unRemained     = _ulPeriodInterval - (ulCurTime % _ulPeriodInterval);
      unRemainedRate  = unRemained*100/_ulPeriodInterval;
      unDiff = abs((int)(ulPeriodIndex - _ulPrevPeriodIndex)); 
      _ulPrevPeriodIndex = ulPeriodIndex;
      unCurrentIndex = ulPeriodIndex % _unPeriodBlockCount; 


      if(unDiff == 0) {
         //do nothing
      } else if(unDiff < _unPeriodBlockCount) { //if skip priod  
         while(--unDiff) { // if skip period,   period_sum = 0
            unsigned int unIndex = abs((int)(ulPeriodIndex-unDiff))%_unPeriodBlockCount;
            _unIntervalSumIn -= _unarrPeriodSumIn[unIndex];
            _unarrPeriodSumIn[unIndex] = 0;

            _unIntervalSumOut -= _unarrPeriodSumOut[unIndex];
            _unarrPeriodSumOut[unIndex] = 0;
         }

         _unHeadPeriodSumIn =  _unarrPeriodSumIn[unCurrentIndex];
         _unIntervalSumIn  -=  _unarrPeriodSumIn[unCurrentIndex];
         _unarrPeriodSumIn[unCurrentIndex] = 0;

         _unHeadPeriodSumOut =  _unarrPeriodSumOut[unCurrentIndex];
         _unIntervalSumOut  -=  _unarrPeriodSumOut[unCurrentIndex];
         _unarrPeriodSumOut[unCurrentIndex] = 0;
      } else { // if skip all period 
         memset(_unarrPeriodSumIn, 0, sizeof(unsigned int) * _unPeriodBlockCount);
         _unIntervalSumIn = 0;
         _unHeadPeriodSumIn = 0;

         memset(_unarrPeriodSumOut, 0, sizeof(unsigned int) * _unPeriodBlockCount);
         _unIntervalSumOut = 0;
         _unHeadPeriodSumOut = 0;
      }
      _unarrPeriodSumIn[unCurrentIndex] += unLoadBits;
      _unIntervalSumIn                  += unLoadBits;
      _unSumIn                          += load;

      _unCurLoadIn = (_unIntervalSumIn + (_unHeadPeriodSumIn*unRemainedRate/100)) / 1;
      if (_unPeakLoadIn < _unCurLoadIn)
         _unPeakLoadIn=_unCurLoadIn;


      unTmpLoadOut = (_unIntervalSumOut + unLoadBits + (int)(_unHeadPeriodSumOut * unRemainedRate / 100)) / 1;
#if 1
      if(max_load > 0 && unTmpLoadOut > max_load) { // drop
         _unCurLoadOut = (_unIntervalSumOut + (int)(_unHeadPeriodSumOut * unRemainedRate / 100)) / 1;
         if (_unPeakLoadOut < _unCurLoadOut)
            _unPeakLoadOut=_unCurLoadOut;
         rcps =_unCurLoadOut; 
         //_pLock->unlock(); //////////////
         return false;
      } else  {
         _unCurLoadOut = unTmpLoadOut;
         if (_unPeakLoadOut < _unCurLoadOut)
            _unPeakLoadOut=_unCurLoadOut;
      }
#else
      bool bRet=true;
      if(max_load > 0 && unTmpLoadOut > max_load) { // drop
         bRet=false;
      }
      _unCurLoadOut = unTmpLoadOut;
      if (_unPeakLoadOut < _unCurLoadOut)
         _unPeakLoadOut=_unCurLoadOut;

#endif
      _unSumOut                          += load;
      _unIntervalSumOut                  += unLoadBits;
      _unarrPeriodSumOut[unCurrentIndex] += unLoadBits;

      rcps =_unCurLoadOut; 
      //_pLock->unlock(); //******* unlock
      return true;
   }

   bool operator() (unsigned int load, unsigned int& rcps ,unsigned int max_load)
   {
      return calcLoad(load, rcps , max_load);
   }

protected:
   unsigned long _currentTime(){
      struct timeval tv;
      gettimeofday(&tv, NULL);
      return  ((unsigned long)(tv.tv_sec) * 1000) + ((unsigned long)tv.tv_usec/1000);
   }

protected:
   // <-- load data
   //unsigned long        _ulStartTime;
   unsigned int         _unCurLoadIn;   //current load with input load
   unsigned int         _unCurLoadOut;  //current load without input load
   unsigned int         _unSumIn;    
   unsigned int         _unSumOut;   

   unsigned long        _ulInterval;            //must be multiple of _unPeriodBlockCount
   unsigned int         _unPeriodBlockCount;    //
   unsigned long        _ulPeriodInterval;
   unsigned long        _ulPrevPeriodIndex;

   unsigned int         _unPeakLoadIn;
   unsigned int         _unIntervalSumIn; // 
   unsigned int         _unHeadPeriodSumIn; // unRemainPeriodValue
   unsigned int         _unarrPeriodSumIn[E_MAX_PERIOD_BLOCK];

   unsigned int         _unPeakLoadOut;
   unsigned int         _unIntervalSumOut; // 
   unsigned int         _unHeadPeriodSumOut; // unRemainPeriodValue
   unsigned int         _unarrPeriodSumOut[E_MAX_PERIOD_BLOCK];
   // --> load data

   PMutex               _mutex;
};



#endif //__PLOAD_CONTROL_H__

