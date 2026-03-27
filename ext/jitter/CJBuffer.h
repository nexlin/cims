#ifndef _PACKET_JITTER_BUFFER_H_V02_ 
#define _PACKET_JITTER_BUFFER_H_V02_

#include "pi_basic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

namespace PICO {

unsigned long GetNowTimestampMS(void);
unsigned long GetNowTimestampUS(void);

/* max frames for framelist */
#define JB_FRAME_LIST_MAX    (100) // 2 seconds.
/* one frame's max size     */
//#define JB_FRAME_CONTENT_MAX (200) // for 224 mux data limit. 
#define JB_FRAME_CONTENT_MAX (640) // for 224 mux data limit. 

/* Number of OP switches to be performed in JB_STATUS_INITIALIZING, before
 * JB can switch its states to JB_STATUS_PROCESSING.
 */
#define JB_INIT_CYCLE              10

/* Maximum burst length, whenever an operation is bursting longer than
 * this value, JB will assume that the opposite operation was idle.
 */
#define JB_MAX_BURST_MSEC          1000

/* The constant JB_DEFAULT_INIT_DELAY specifies default jitter
 * buffer prefetch count during jitter buffer creation.
 */
#define JB_DEFAULT_INIT_DELAY    15

#define JB_MAXDROPOUT_MIN       (150 ) // about 3 seconds.
#define JB_MAXDROPOUT_MAX       (3000) //

#define JB_MAXMISORDER_MIN       1     //
#define JB_MAXMISORDER_MAX      (100) 

// log print
#define JBTL_INF (1)
#define JBTL_API (2)
#define JBTL_DBG (3)


typedef struct tag_JBParam_T{
    pibyte UseAdaptive  ; // 0: FJB mode, 1: AJB mode
                          // In FJB mode, minprefetch used as prefetch count.
    pibyte AllowUnderMin; // In FJB mode, this value set 0.
    pibyte reserved2    ;
    pibyte reserved3    ;

    pisb32 PTime        ;
    pisb32 DropoutMax   ;
    pisb32 MisorderMax  ;
    
    pisb32 MinPrefetch  ; 
    pisb32 MaxPrefetch  ;
    pisb32 InitPrefetch ;
    pisb32 BufferMax    ; // FrameList Parameter. 
    pisb32 BurstMax     ; //BurstMax MAX(BufferMaxMsec/ptime,BufferMax 75%);

    piub32 DropsOnResShort;// Put Resource Shortage occurred. how must drop ?

    pibyte UseTsAsIndex   ; // rtp timestamp used by buffer index 
    pibyte UseAdjust      ; // Use Adjust scheme.

    piub32 CSID           ; // call session id for log output

    void Reset(void){
        UseAdaptive  = 0;
        AllowUnderMin= 0;
        PTime        =20;
        DropoutMax   =JB_MAXDROPOUT_MIN ;
        MisorderMax  =JB_MAXMISORDER_MIN; 
        MinPrefetch  = 0 ;
        MaxPrefetch  = 0 ;
        InitPrefetch = 0 ;
        BufferMax    = 0 ;
        BurstMax     = 0 ;

        DropsOnResShort= 0 ;

        UseTsAsIndex = 0 ;
        UseAdjust    = 0 ;

        CSID         = 0xFFFFFFFF ;
    }

}JBParam_T;

typedef enum E_JBFrameType_T{
    E_JBFrameType_Missing     = 0,
    E_JBFrameType_Normal      = 1,
    E_JBFrameType_ZeroPrefetch= 2,
    E_JBFrameType_ZeroEmpty   = 3,
    E_JBFrameType_Discarded   =1024
};

struct JBFrame_T{
    E_JBFrameType_T type       ; // Missing, Normal, Discarded(now not used)
    unsigned long   rxtime_msec; // real received time milisecs

    // RTP packet description
    pibyte pt         ; // original rtp payload type
    pibyte marker     ; // original rtp marker bit
    pibyte isdtmf     ; // hint is DTMF payload ?
    pibyte isspeech   ; // frame is speech ?
    piub16 seqnum     ; // original rtp packet sequence number
    piub32 timestamp  ; // original rtp packet timestamp

    piub16 contentlen ; // total rtp packet's size
    pibyte content[JB_FRAME_CONTENT_MAX];

    void Init(void){
        type       = E_JBFrameType_Missing ;
        rxtime_msec= 0 ;
        contentlen = 0 ;
        pt         = 0 ;
        marker     = 0 ;
        isdtmf     = 0 ;
        seqnum     = 0 ;
        timestamp  = 0 ;
    }
};

// RTP Frame Description
typedef struct JBFrameDesc_T{
    unsigned long   rxtime_msec;

    E_JBFrameType_T frmtype    ;
    piub16          pktlen     ;
    piub16          seqnumber  ;
    piub32          timestamp  ;
    pibyte          pt         ;
    pibyte          marker     ; 
    pibyte          isdtmf     ;
    pibyte          isspeech   ; // frame is speech frame ?

    pisb32          index      ; // index value
    //pibyte       *data       ;
};

typedef struct JBFLStatsEnt_T{
    piub32 PutTooLateSID    ; // Put SID return Too Late
    piub32 PutTooLateSpeech ; // Put Speech return Too Late 
    piub32 PutResShortSID   ; // Put SID return Resource shortage
    piub32 PutResShortSpeech; // Put Speech return Resource shortage
    piub32 PutDupSID        ; // Put SID return duplicated
    piub32 PutDupSpeech     ; // Put Speech return duplicated
    piub32 PutSID           ; // Put SID Frame success count
    piub32 PutSpeech        ; // Put Speech Frame success count 

    piub32 RemoveSpeech     ; // Remove Normal Speech frame
    piub32 RemoveSID        ; // Remove Normal SID frame
    piub32 RemoveMissing    ; // Remove Missing Frame
    piub32 RemoveDiscarded  ; // Remove Discarded Frame

    piub32 GetSpeech        ;
    piub32 GetSID           ;
    piub32 GetMissing       ;
    piub32 GetZeroEmpty     ;

    piub32 ResetBackward    ;
    piub32 ResetForward     ;
    piub32 OriginResetZeroSize;

    piub32 ResetDropSpeech  ;
    piub32 ResetDropSID     ;

    void Reset(void){

        PutTooLateSID=0;
        PutTooLateSpeech=0;
        PutResShortSID=0;
        PutResShortSpeech=0;
        PutDupSID=0;
        PutDupSpeech=0;
        PutSID=0;
        PutSpeech=0;

        RemoveSpeech=0;
        RemoveSID=0;
        RemoveMissing=0;
        RemoveDiscarded=0;

        GetSpeech=0;
        GetSID=0;
        GetMissing=0;
        GetZeroEmpty=0;

        ResetBackward=0;
        ResetForward=0;
        OriginResetZeroSize=0;

        ResetDropSpeech = 0 ;
        ResetDropSID    = 0 ;
    }
};

struct CJBFrameList{
    piub32 CSID             ; // for log trace output
    piub32 m_MaxCount       ; // parameter
    piub32 m_MaxFrameSize   ; // parameter
    pisb32 m_MaxDropout     ; // parameter
    pisb32 m_MaxMisorder    ; // parameter

    pisb32 m_Origin         ; // first node's original sequence number

    piub32 m_HeadIdx        ; // first node's idx
    piub32 m_Size           ; // list frames count including missing frame.
    piub32 m_DiscardedNum   ; // discarded number.

    JBFLStatsEnt_T m_Stats  ;

    JBFrame_T m_Array[JB_FRAME_LIST_MAX];

    PiResult_T Init(JBParam_T*param);

    PiResult_T Reset(pibyte isinitial=0);

    piub32     Size(void)   { return m_Size ; } ;
    piub32     EffSize(void){ return m_Size - m_DiscardedNum ; }
    pisb32     Origin(void) { return m_Origin ; }
    piub32     MaxCount(void){ return m_MaxCount ; }
    piub32     MaxFrameSize(void){ return m_MaxFrameSize;}

    PiResult_T Peek(piub32 offset, // list order
            JBFrameDesc_T*odesc,
            pibyte*oFrameBuffer,
            piub32 FrameBufferLen);

    PiResult_T Get(
            JBFrameDesc_T*odesc,
            pibyte*oFrameBuffer,
            piub32 FrameBufferLen);

    piub32 RemoveHead(piub32 count);

    PiResult_T PutAt(
        JBFrameDesc_T*odesc,
        pibyte*frame,
        pibyte*oreseted);

    PiResult_T Discard(int seq);

    piub32 Adjust(piub32 minprefetch,pisb32 curindex);

    void LogPrint(piub32 level,char*fmt,...);
};

typedef enum E_CJBState_T{
    E_CJBState_Initializing=0,
    //E_CJBState_Prefetching =1,
    E_CJBState_Progressing =2
};

typedef enum E_CJBOper_T{
    E_CJBOper_Init=0,
    E_CJBOper_Put =1,
    E_CJBOper_Get =2
};

typedef struct _tagJBStatsEnt_T{ 
    piub32 SeqRestarted        ; // RTP sequence restarted count
    piub32 SeqReceived         ; // total received RTP packets
   
    piub32 SeqFwJump           ; // sequence forward jump in window
    piub32 SeqDuplicated       ; // duplicated packet 
    piub32 SeqMisordered       ; // backward window.
    piub32 SeqMisorderCorrected; // corrected by JBuffer
    piub32 SeqMisorderDroped   ; // can't correct
    piub32 SeqDiscarded        ; // put discarded return

    piub32 JDLevel1[10]        ;
    piub32 JDLevel2[10]        ;
    piub32 JDLevelOver         ;
    piub32 JDLevelForgot       ;
    piub32 JDHighMark          ;


    piub32 GetCalled           ;

    piub32 GetSuccess          ; // GetFrame call FL Get return value
    piub32 GetErrAgain         ; // GetFrame call FL Get return value
    piub32 GetSysfail          ;

    piub32 GetDriftLevel[10]   ;
    piub32 GetDriftLevelOver   ;
    piub32 GetDriftHighMark    ;// 1 Get >= 6 count

/*
    piub32 empty    ;
    piub32 discarded;
    piub32 lost     ;
*/

    void Reset(void){
        SeqRestarted         = 0 ;
        SeqReceived          = 0 ;
        SeqMisordered        = 0 ;
        SeqMisorderCorrected = 0 ;
        SeqMisorderDroped    = 0 ;
        SeqDiscarded         = 0 ;
        JDLevelForgot        = 0 ;

        for(int i = 0 ; i < 10 ; i++){
            JDLevel1[i] = JDLevel2[i]=0;
        }
        JDLevelOver = 0;
        JDHighMark  = 0;

        for(int i = 0 ; i < 10 ; i++){
            GetDriftLevel[i] = 0 ;
        }
        GetDriftLevelOver = 0 ;
        GetDriftHighMark  = 0 ;

#if 0
        empty    = 0;
        discarded= 0;
        lost     = 0;
#endif
    }
    void UpdateJDLevel(piub32 jittervalue){
        piub32 idx ;

        if(jittervalue<50){
            idx = jittervalue/5 ;
            JDLevel1[idx%10]++; 
        }else if(jittervalue < 150){
            idx = (jittervalue-50)/10 ;
            JDLevel2[idx%10]++; 
        }else{
            JDLevelOver++;
        }

        if(jittervalue > JDHighMark){
            JDHighMark = jittervalue ;
        }
    }

    void UpdateGetDriftLevel(piub32 count){
        if(count<10){
            GetDriftLevel[count]++;
        }else{
            GetDriftLevelOver++;
        }
        if(count > GetDriftHighMark){
            GetDriftHighMark = count ;
        }
    }

}JBStatsEnt_T ;

typedef struct _tag_JBReport_T{

    struct _tag_JBFLStatus_T{
        piub32 CSID             ; // for log trace output
        piub32 m_MaxCount       ; // parameter
        piub32 m_MaxFrameSize   ; // parameter
        pisb32 m_MaxDropout     ; // parameter
        pisb32 m_MaxMisorder    ; // parameter

        pisb32 m_Origin         ; // first node's original sequence number

        piub32 m_HeadIdx        ; // first node's idx
        piub32 m_Size           ; // list frames count including missing frame.
        piub32 m_DiscardedNum   ; // discarded number.
    }JBFLStatus ;

    struct _tag_JBStatus_T{
        // status value
        piub32 m_PutFrameCount      ;// Put Frame Count
        piub16 m_PutFrameRtpSeqMax  ;// diff for calculate
                                 // forward diff : 
                                 //     idx=framelist's origin + diff
                                 // misorder diff:
                                 //     idx=framelist's origin - diff 
        unsigned long m_LastPutFrameRxTime ;
        unsigned int  m_LastPutFrameRtpTs  ;
        unsigned int  m_LastPutFrameRtpIsSpeech;
        E_CJBState_T m_State         ;
        pisb32       m_InitCycleCnt  ;
        pibyte       m_Prefetching   ; // Is Prefetching ? 
        pisb32       m_Level         ; 
        E_CJBOper_T  m_LastOper ;
        pisb32       m_MaxHistLevel  ;
        pisb32       m_StableHist    ;
        pisb32       m_EffLevel      ;
        pisb32       m_Prefetch      ;
        pisb32       m_CurIndex      ;
    }JBStatus ;

    JBFLStatsEnt_T JBFLStats;
    JBStatsEnt_T   JBStats ;

}JBReport_T ;

struct CJBuffer{

    // sequence control
    struct _tag_Config{
        pisb32 MaxDropout   ;  // drop out range
        pisb32 MaxMisorder  ;  // mis order range
        pisb32 FramePTime   ;  // Frame's P time
        pisb32 InitPrefetch ;  // First Prefetching Count
        pisb32 MinPrefetch  ;  // AJB Min Prefetch count
        pisb32 MaxPrefetch  ;  // AJB Max Prefetch count
        pisb32 MaxBurst     ;  // Max Burst Level. Burst Level over this limit.
                               // not monitoring network burst level.
        pisb32 DropsOnResShort;// Put Resource Shortage occurred. how must drop
        pibyte AllowUnderMin;  // allow prefetch value to be under minprefetch
        pibyte UseTsAsIndex ;
        pibyte UseAdjust    ; // adjust scheme use flag
        piub32 CSID         ; // for trace id
    }m_Config ;

    // status value
    piub32 m_PutFrameCount      ;// Put Frame Count
    piub16 m_PutFrameRtpSeqMax  ;// diff for calculate
                                 // forward diff : 
                                 //     idx=framelist's origin + diff
                                 // misorder diff:
                                 //     idx=framelist's origin - diff 

    unsigned long m_LastPutFrameRxTime ;
    unsigned int  m_LastPutFrameRtpTs  ;
    unsigned int  m_LastPutFrameRtpIsSpeech;

    E_CJBState_T m_State         ;
    pisb32       m_InitCycleCnt  ;
    pibyte       m_Prefetching   ; // Is Prefetching ? 
    pisb32       m_Level         ; 
    E_CJBOper_T  m_LastOper ;
    pisb32       m_MaxHistLevel  ;
    pisb32       m_StableHist    ;
    pisb32       m_EffLevel      ;
    pisb32       m_Prefetch      ;

    pisb32       m_CurIndex      ;

    CJBFrameList m_FrameList     ;
    JBStatsEnt_T m_Stats         ;

    PiResult_T Init(JBParam_T*param);
    void Reset(pibyte isinitial);

    void CalculateJitter(void); 
    void Update(E_CJBOper_T op);

    // result :
    //     successs : 
    //     already  : duplicated packet
    //     toolate  : packet too late due to mis order
    //     invalidp : sysfailure.
    //     TooBig   : sysfailure.
    //     Error    : sysfailure.
    //     resshort : sysfailure.
    PiResult_T PutFrame(
        pibyte isspeechzone,
        JBFrameDesc_T*desc,
        pibyte*frame,
        pibyte*odiscarded);
#if 0
    void PeekFrame(
        piub32 offset,
        JBFrameDesc_T*odesc,
        pibyte*oFrameBuffer,
        piub32 FrameBufferLen);
#endif

    // result :
    //     success  : frame type normal ok. need data
    //     invalidp : sysfailure.
    //     TooBig   : sysfailure.
    PiResult_T GetFrame(
        JBFrameDesc_T*odesc,
        pibyte*oFrameBuffer,
        piub32 FrameBufferLen);

    pibyte IsPrefetching(void){
        if(m_Prefetching){
            return 1 ;
        }else{
            return 0 ;
        }
    }

    // now prefetching and size zero
    pibyte SIDBypassAble(void){

        if(m_Prefetching && ((m_FrameList.Size())==0)){
            return 1;
        }

        return 0 ;
    }

    void LogPrint(piub32 level,char*fmt,...);
    void LogPrint(piub32 level,piub32 mask,char*fmt,...);
    void LogDump(piub32 levvel,piub32 mask);

    /* printlevel: 0, not print. return immediately.
     *             1, print stats information
     *             2, print stats information and JD level, Get Drift Level information
     */
    //void LoggingStats(pisb32 printlevel);

    void GetReport(JBReport_T*oreport);
};

typedef struct _tag_JBZoneTrace{
    piub32        CSID            ;
    piub32        TalkSepMSec     ;

    piub32        SpeechZoneCount ;
    piub32        SilientZoneCount; 

    pibyte        IsSpeechZone ; // 1: speech zone, 0: silience zone

    unsigned long ZoneStartMSec ;  // real time
    unsigned long LatestSpeechPlayMSec ;  // real time

    void Reset(void){
    //    TalkSepMSec      = 800;
        SpeechZoneCount  = 0 ;
        SilientZoneCount = 0 ;

        IsSpeechZone  = 0 ;
        ZoneStartMSec     = 0 ;
        LatestSpeechPlayMSec=0;
    }

    void Init(piub32 csid,piub32 talksepmsec){
        CSID = csid ;
        if(talksepmsec<=0){
            talksepmsec = 1000*60 ;
        }else if(talksepmsec>1000*60*15){
            talksepmsec = 1000*60 ;
        }else{
            TalkSepMSec = talksepmsec ;
        }
    }

    //void LogPrint(piub32 level,char*fmt,...);
    void Update(pibyte speech);

}JBZoneTrace_T ;

void jb_change_tlog_level(pisb32 level);

}; // PICO


#endif

