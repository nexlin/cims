#ifndef  __AMT_RTP_DTMF_H_
#define  __AMT_RTP_DTMF_H_

#include <sys/time.h>
#include <math.h>
#include <map>
#include <list>

#include "jthread.h"
#include "amtrtp.h"
#include "amtrtpcomm.h"


namespace AMT
{

enum eDtmfType{
  DT_NONE           = 0x0000,
  DT_INBAND_START   = 0x0001,
  DT_INBAND_END     = 0x0002,
  DT_RFC2833_START  = 0x0003,
  DT_RFC2833_END    = 0x0004,
  DT_ALL            = 0x00FF
};

enum eDtmfCode{
  DTMF_0            =  0,
  DTMF_1            =  1,
  DTMF_2            =  2,
  DTMF_3            =  3,
  DTMF_4            =  4,
  DTMF_5            =  5,
  DTMF_6            =  6,
  DTMF_7            =  7,
  DTMF_8            =  8,
  DTMF_9            =  9,
  DTMF_STAR         = 10, // *
  DTMF_PND          = 11, // #
  DTMF_A            = 12,
  DTMF_B            = 13,
  DTMF_C            = 14,
  DTMF_D            = 15,
  DTMF_C11          = 16, // +C11       700 + 1700
  DTMF_C12          = 17, // +C12       900
  DTMF_KP1          = 18, // KP1+       1100
  DTMF_KP2          = 19, // KP2+       1300
  DTMF_ST           = 20, // +ST        1500
  DTMF_24           = 21, // 2400
  DTMF_26           = 22, // 2600
  DTMF_2426         = 23, // 2400+2600
  DTMF_DT           = 24, // DialTone.  350 440
  DTMF_RING         = 25, // Ring       440 480
  DTMF_BUSY         = 26, // Busy       480 620
  DTMF_SIL          = 27, // Silence
  DTMF_NONE         = 28, // Nothing.
  TONE_DIAL         = 29, // ToneDial
  TONE_RINGBACK     = 30, // ToneRingback
  TONE_BUSY         = 31, // ToneBusy
  TONE_CONGESTION   = 32, // ToneCongestion
  TONE_WAITING      = 33, // ToneWaiting
  TONE_HOLDING      = 34, // ToneHolding
  TONE_INTERCEPT    = 35, // ToneIntercept
  TONE_SPECIALDIAL  = 36, // ToneSpecialDial
  TONE_CONFIRM      = 37, // ToneConfirm
  TONE_HOLWER       = 38, // ToneHowler
  TONE_SIT_RO1      = 39, // ToneSITReorder1
  TONE_SIT_VC       = 40, // ToneSITVacantCode
  TONE_SIT_NC1      = 41, // ToneSITNoCircuit1
  TONE_SIT_IC       = 42, // ToneSITIntercept
  TONE_SIT_RO2      = 43, // ToneSITReorder2
  TONE_SIT_NC2      = 44, // ToneSITNoCircuit2
  TONE_SIT_IO       = 45, // ToneSITIneffectiveOther
  TONE_END          = 46, // End of tone.
};

enum eDtmfOpcode
{
    OP_DTMF_NONE            = 0x0,
    OP_DTMF_NOTIFY          = 0x01,
    OP_DTMF_GEN             = 0x02,
    OP_DTMF_GEN_CON         = 0x03,
    OP_DTMF_GEN_2833        = 0x04,
    OP_DTMF_GEN_2833_CON    = 0x05,
    OP_DTMF_MAX
};

enum { RTP_PAYLOAD_RFC2833 = 101};

typedef struct {
#if(HUMT_GCONF_BIG_ENDIAN)
   unsigned int event: 8;
   unsigned int edge: 1;
   unsigned int reserved: 1;
   unsigned int volume: 6;
   unsigned int duration: 16;
#else  // LITTLEENDIAN
   unsigned int event: 8;
   unsigned int volume: 6;
   unsigned int reserved: 1;
   unsigned int edge: 1;
   unsigned int duration: 16;
#endif
}RtpPayloadRfc2833;

typedef struct {
   char bEnable;
   unsigned char ucPayloadType;
   unsigned short usSeq;         // if first gen, set 0
   int  nRtpPort;
   unsigned int uTimestamp;      // if first gen, set 0
   unsigned int uTimeScale;
   unsigned int lFirstStartTimeMS;
}DTMFGEN_STS;//dtmf generate status

#define MAX_DTMFGEN_DURATION 8000//ms 8sec max duration internal setting for prevent error case
                                //rfc 2833 durtaion field have 16bit size, so have 8sec max limit

#define DTMF_GENPARM_NUM 7
#define AMT_DTMFGEN_PARM_ALL (AMT::DTMFGEN_PARM_ENABLE | AMT::DTMFGEN_PARM_RTPPORT | \
   AMT::DTMFGEN_PARM_PT | AMT::DTMFGEN_PARM_FSTIME | AMT::DTMFGEN_PARM_SEQ |  \
   AMT::DTMFGEN_PARM_TSTAMP | AMT::DTMFGEN_PARM_TSCALE)
enum eDtmfGenParm{
    DTMFGEN_PARM_BASE           = 0x1000,
   DTMFGEN_PARM_ENABLE        = DTMFGEN_PARM_BASE | 0x0001,
   DTMFGEN_PARM_RTPPORT       = DTMFGEN_PARM_BASE |0x0002,
   DTMFGEN_PARM_PT            = DTMFGEN_PARM_BASE |0x0004,
   DTMFGEN_PARM_FSTIME        = DTMFGEN_PARM_BASE |0x0008,//first start time
   DTMFGEN_PARM_SEQ           = DTMFGEN_PARM_BASE |0x0010,
   DTMFGEN_PARM_TSTAMP        = DTMFGEN_PARM_BASE |0x0020,//timestamp
   DTMFGEN_PARM_TSCALE        = DTMFGEN_PARM_BASE |0x0040,
   DTMFGEN_PARM_MAX           = DTMFGEN_PARM_BASE |0x0080
};

typedef struct {
   char bEnable; // default false
   char bFirst;//dtmf det First
   unsigned char ucPayloadType;
   unsigned char ucLastDigit;
   unsigned int uLastDuration;
   unsigned int uLastTimestamp;
   unsigned int uTimeScale;
} DTMFDET_STS;//dtmf detect status

#define DTMF_DETPARM_NUM 6
#define AMT_DTMFDET_PARM_ALL (AMT::DTMFDET_PARM_ENABLE | AMT::DTMFDET_PARM_PT | \
   AMT::DTMFDET_PARM_LAST_DIGIT | AMT::DTMFDET_PARM_LAST_DURATION | \
   AMT::DTMFDET_PARM_LAST_TSTAMP | AMT::DTMFDET_PARM_TSCALE)
enum eDtmfDetParm{
    DTMFDET_PARM_BASE           = 0x2000,
   DTMFDET_PARM_ENABLE        = DTMFDET_PARM_BASE | 0x0001,
   DTMFDET_PARM_PT            = DTMFDET_PARM_BASE | 0x0002,
   DTMFDET_PARM_LAST_DIGIT    = DTMFDET_PARM_BASE | 0x0004,
   DTMFDET_PARM_LAST_DURATION = DTMFDET_PARM_BASE | 0x0008,
   DTMFDET_PARM_LAST_TSTAMP   = DTMFDET_PARM_BASE | 0x0010,//timestamp
   DTMFDET_PARM_TSCALE        = DTMFDET_PARM_BASE | 0x0020,
   DTMFDET_PARM_MAX           = DTMFDET_PARM_BASE | 0x0040
};

#define AMT_DTMF_PARM_ALL (AMT_DTMFGEN_PARM_ALL | AMT_DTMFDET_PARM_ALL ) 

typedef struct DtmfDetInfo{
  unsigned short usType;           // MTDtmf::DTMF_TYPE, DT_ALL(X)
  unsigned short usDigit;       // DTMF_CODE
  unsigned short usDuration;   // milli-second
  unsigned short usPower;      // max 100
  unsigned short usRtpPayloadType; // 
}DtmfDetInfo ;

typedef struct DtmfGenInfo{
    bool bFirst;
    unsigned short usType;
    unsigned short usDigit;
    unsigned short usDuration;
    unsigned short usVolume;
    unsigned short usMarker;
    unsigned short usEndCnt;
    unsigned int lStartTimeMS;
    unsigned int lEndTimeMS;
    unsigned int lCurTimeMS;
    DtmfGenInfo() {
      memset(this,0x00,sizeof(DtmfGenInfo));
      bFirst = true;
   }
}DtmfGenInfo;

typedef bool (*RtpDtmfCallBack)(void* pUserClass,void * pParams);


//sigleton design pattern
#define sRtpDtmfSender (AMT::CRtpDtmfSender::GetInstance())

class CRtpDtmf;

//typedef std::list<DtmfGenInfo> ListDtmfGenInfo;
typedef std::map<CRtpDtmf*,DtmfGenInfo> MapDtmfGenInfo;

class CRtpDtmfSender : public CThread
{
public:
    CRtpDtmfSender();
    ~CRtpDtmfSender();
    enum { PERIOD_SND_DTMF = 20 }; //milli-sec
    enum { MAX_END_PKT = 3 }; // count 

    static CRtpDtmfSender* GetInstance();

    bool Add(CRtpDtmf* pSender, int nDigit, int nDuration, int nVolume);
    //bool Remove(CRTPSender* pSender); 
    bool Init(int nIntervalMS=PERIOD_SND_DTMF, int nEndPktCnt=MAX_END_PKT);
    bool Start(){return Create();}
    void Stop(){Close();}
protected:
    static CRtpDtmfSender* m_pInstance;

    void * ThreadProc();
    int m_nInterval;
    int m_nMaxEndPkt;

    CCritSec     m_Lock;
    //CHuMutex     m_Lock;
    MapDtmfGenInfo m_Map;

    //CLogger    * m_pLogger;
};


class CRtpDtmf
//RtpDtmf is desinged for treating dtmf event that occur at each rtp session
//rtpcomm session will have rtpdtmf_class with member for handle each session event
//low level dtmf handle procedure must be define in this class.. need move some code
//  relate dtmf to here.. in some project in some process.. 
{

public:
   CRtpDtmf(CRtpComm* CRtpComm);
   ~CRtpDtmf();
   bool Init();//Init all Params when session restart

    bool GenInit(unsigned short usSeq,unsigned int lTimestamp);
   bool RegistCallBack(eDtmfOpcode nOpcode,RtpDtmfCallBack pUserFunc,void* pUserClass);
  

   int SetParams(unsigned int nParmType,void * pParams);
   void* GetParams(unsigned int nParmType);

    int GenDTMF(int nOpcode,int nDigit, int nDuration, int nPower);
   int HandleGenDtmf(unsigned int nOpCode,DtmfGenInfo* pDtmfInfo,
        int nEndCnt,bool bForceEnd=false);
    int HandleDetDtmf2833(char *pPayload,int nDataLen,RTPHEADER* pRtpHdr);

   //int DtmfCodeTo2833(int nDtmfCode);
   //int Dtmf2833ToCode(int nDtmf2833);
   inline char DtmfCodeToChar(int nDtmfCode)
    {
       switch ((eDtmfCode)nDtmfCode) {
       case DTMF_0: return (char)'0';
       case DTMF_1: return (char)'1';
       case DTMF_2: return (char)'2';
       case DTMF_3: return (char)'3';
       case DTMF_4: return (char)'4';
       case DTMF_5: return (char)'5';
       case DTMF_6: return (char)'6';
       case DTMF_7: return (char)'7';
       case DTMF_8: return (char)'8';
       case DTMF_9: return (char)'9';
       case DTMF_STAR: return (char)'*';
       case DTMF_PND: return (char)'#';
       case DTMF_A: return (char)'A';
       case DTMF_B: return (char)'B';
       case DTMF_C: return (char)'C';
       case DTMF_D: return (char)'D';
       default: break;
       }

       return 0;
    }
    //inline ListDtmfGenInfo* GetListDtmfGenInfo(){return &m_lsDtmfGenInfo;}
   
protected:

   DTMFDET_STS m_stDtmfDetSts;
   DTMFGEN_STS    m_stDtmfGenSts;
    CRtpComm* m_pRtpComm;
   //ListDtmfGenInfo m_lsDtmfGenInfo;
   RtpDtmfCallBack m_RtpDtmfNotifyCallBack;
   RtpDtmfCallBack m_RtpDtmfGenCallBack;
   void* m_pUserClass;
};


}; //namespace

#endif   //__AMT_RTP_DTMF_H__
/////@}

