#ifndef __PI_BASIC_ERRORS_H_V01_
#define __PI_BASIC_ERRORS_H_V01_


typedef enum {
    PiSuccess          = 1,
    PiRSuccess         = 1,
    PiError            =-1,
    PiRError           =-1,
    PiRErrInvalidParam =-2, 
    PiRErrInit         =-3, 
    PiRErrMutexInit    =-4, 
    PiRErrCondVarInit  =-5, 
    PiRErrSpinInit     =-6, 
    PiRErrRWLockInit   =-6,
    PiRErrLock         =-7, 
    PiRErrUnlock       =-8,
    PiRErrEmptyList    =-9,
    PiRErrAgain        =-10, 
    PiRErrUnknown      =-11,
    PiRErrNotYet       =-12,
    PiRErrListBadLink  =-13,
    PiRErrListBadUser  =-14,
    PiRErrTooBig       =-15, 
    PiRErrMalloc       =-16,
    PiRErrNotAllowed   =-17,
    PiRErrMutexDest    =-4, 
    PiRErrCondVarDest  =-5, 
    PiRErrSpinDest     =-6, 
    PiRErrResShortage  =-20,
    PiRErrSysfail      =-1000,
    PiRErrShmGet       =-30,
    PiRErrAlready      =-31,
    PiRErrShmStat      =-32,
    PiRErrShmAttach    =-33,
    PiRErrShmDetach    =-34,
    PiRErrShmRemove    =-35,
    PiRErrBadMagic     =-36,
    PiRErrOpen         =-36,
    PiRErrNeedInit     =-36,
    PiRErrNoSuch       =-37,
    PiRErrNoMore       =-38,
    PiRErrTimeout      =-39,
    PiRErrTooLate      =-40,
    PiRErrJBSysfail    =-41
}PiResult_T ;





#endif
