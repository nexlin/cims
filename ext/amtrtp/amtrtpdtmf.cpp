#include <stdlib.h>
#include "amtrtpdtmf.h"
#include "destroyer.h"
#include "amrtpbuffer.h"


namespace AMT
{

CRtpDtmf::CRtpDtmf(CRtpComm *pRtpComm) 
   : m_RtpDtmfNotifyCallBack(NULL),
     m_RtpDtmfGenCallBack(NULL), 
     m_pUserClass(NULL)
{
   m_pRtpComm = pRtpComm;
   memset(&m_stDtmfDetSts,0x00,sizeof(m_stDtmfDetSts));
   memset(&m_stDtmfGenSts,0x00,sizeof(m_stDtmfGenSts));
}

CRtpDtmf::~CRtpDtmf(){}

bool CRtpDtmf::Init()
{
   memset(&m_stDtmfDetSts,0x00,sizeof(m_stDtmfDetSts));
   memset(&m_stDtmfGenSts,0x00,sizeof(m_stDtmfGenSts));
   return true;
}

bool CRtpDtmf::GenInit(unsigned short usSeq,unsigned int lTimestamp)
{
   m_stDtmfGenSts.usSeq = usSeq;
   m_stDtmfGenSts.uTimestamp = lTimestamp;

   timeval tval;
   gettimeofday(&tval,NULL);//
   m_stDtmfGenSts.lFirstStartTimeMS = tval.tv_sec *1000 + tval.tv_usec/1000;
   return true;
}

bool CRtpDtmf::RegistCallBack(eDtmfOpcode nOpcode,RtpDtmfCallBack pUserFunc,void* pUserClass)
{
   if(nOpcode >= OP_DTMF_MAX || nOpcode <= OP_DTMF_NONE)
      return false;

   switch((eDtmfOpcode)nOpcode)
   {
      case OP_DTMF_NOTIFY:
         m_RtpDtmfNotifyCallBack = (RtpDtmfCallBack)pUserFunc;
         break;
      case OP_DTMF_GEN_2833:
         m_RtpDtmfGenCallBack = (RtpDtmfCallBack)pUserFunc;
         break;
      default:
         return false;
         break; 
   }
   m_pUserClass = pUserClass;
   return true;
}

void* CRtpDtmf::GetParams(unsigned int nParmType)
{
   if((nParmType&0xfff) > AMT_DTMF_PARM_ALL)
      return NULL;

   if((nParmType & DTMFGEN_PARM_BASE)==DTMFGEN_PARM_BASE)
   {
      if(nParmType == AMT_DTMFGEN_PARM_ALL)
      {
         return &m_stDtmfGenSts;
      }
      else
      {
         switch((eDtmfGenParm)nParmType|DTMFGEN_PARM_BASE)
         {
            case DTMFGEN_PARM_ENABLE:
               return &m_stDtmfGenSts.bEnable;
               break;
            case DTMFGEN_PARM_RTPPORT:
               return &m_stDtmfGenSts.nRtpPort;
               break;
            case DTMFGEN_PARM_PT:
               return &m_stDtmfGenSts.ucPayloadType;
               break;
            case DTMFGEN_PARM_FSTIME:
               return &m_stDtmfGenSts.lFirstStartTimeMS;
               break;
            case DTMFGEN_PARM_SEQ:
               return &m_stDtmfGenSts.usSeq;
               break;
            case DTMFGEN_PARM_TSTAMP:
               return &m_stDtmfGenSts.uTimestamp;
               break;
            case DTMFGEN_PARM_TSCALE:
               return &m_stDtmfGenSts.uTimeScale;
               break;
            default:
               break;
         }//switch
      }
   }
   else if((nParmType & DTMFDET_PARM_BASE)==DTMFDET_PARM_BASE)
   {
      if(nParmType == AMT_DTMFDET_PARM_ALL)
      {
         return &m_stDtmfDetSts;
      }
      else
      {
         switch((eDtmfGenParm)nParmType|DTMFDET_PARM_BASE)
         {
            case DTMFDET_PARM_ENABLE:
               return &m_stDtmfDetSts.bEnable;
               break;
            case DTMFDET_PARM_PT:
               return &m_stDtmfDetSts.ucPayloadType;
               break;
            case DTMFDET_PARM_LAST_DIGIT:
               return &m_stDtmfDetSts.ucLastDigit;
               break;
            case DTMFDET_PARM_LAST_DURATION:
               return &m_stDtmfDetSts.uLastDuration;
               break;
            case DTMFDET_PARM_LAST_TSTAMP:
               return &m_stDtmfDetSts.uLastTimestamp;
               break;
            case DTMFDET_PARM_TSCALE:
               return &m_stDtmfDetSts.uTimeScale;
               break;
            default:
               break;
         }//switch
      }


   }
   return NULL;
}

int CRtpDtmf::SetParams(unsigned int nParmType, void* pParams)
      //0:success
      //-1:exceed param
      //-2:undefine gen param
      //-3:undefine det param
{
   if(((nParmType & 0xfff)> AMT_DTMF_PARM_ALL) || pParams == NULL)
      return -1;


   if((nParmType & DTMFGEN_PARM_BASE)==DTMFGEN_PARM_BASE)
   {
      unsigned int nOneType;
      DTMFGEN_STS* pDtmfGenSts = (DTMFGEN_STS*)pParams;

      for(int i=0;i<DTMF_GENPARM_NUM;i++)
      {
         nOneType = 1<<i; 
         if((nOneType & nParmType) == nOneType)
         {
            switch((eDtmfGenParm)nOneType|DTMFGEN_PARM_BASE)
            {
               case DTMFGEN_PARM_ENABLE:
                  m_stDtmfGenSts.bEnable = pDtmfGenSts->bEnable;
                  break;
               case DTMFGEN_PARM_RTPPORT:
                  m_stDtmfGenSts.nRtpPort = pDtmfGenSts->nRtpPort;
                  break;
               case DTMFGEN_PARM_PT:
                  m_stDtmfGenSts.ucPayloadType = pDtmfGenSts->ucPayloadType;
                  break;
               case DTMFGEN_PARM_FSTIME:
                  m_stDtmfGenSts.lFirstStartTimeMS = pDtmfGenSts->lFirstStartTimeMS;
                  break;
               case DTMFGEN_PARM_SEQ:
                  m_stDtmfGenSts.usSeq = pDtmfGenSts->usSeq;
                  break;
               case DTMFGEN_PARM_TSTAMP:
                  m_stDtmfGenSts.uTimestamp = pDtmfGenSts->uTimestamp;
                  break;
               case DTMFDET_PARM_TSCALE:
                  m_stDtmfGenSts.uTimeScale = pDtmfGenSts->uTimeScale;
                  break;
               default:
                  break;
            }//switch
         }
      }//for
   }
   else if((nParmType & DTMFDET_PARM_BASE)==DTMFDET_PARM_BASE)
   {
      unsigned int nOneType;
      DTMFDET_STS* pDtmfDetSts = (DTMFDET_STS*)pParams;
      for(int i=0;i<DTMF_DETPARM_NUM;i++)
      {
         nOneType = 1<<i; 
         if((nOneType & nParmType) == nOneType)
         {
            switch((eDtmfDetParm)nOneType|DTMFDET_PARM_BASE)
            {
               case DTMFDET_PARM_ENABLE:
                  m_stDtmfDetSts.bEnable = pDtmfDetSts->bEnable;
                  break;
               case DTMFDET_PARM_PT:
                  m_stDtmfDetSts.ucPayloadType = pDtmfDetSts->ucPayloadType;
                  break;
               case DTMFDET_PARM_LAST_DIGIT:
                  m_stDtmfDetSts.ucLastDigit = pDtmfDetSts->ucLastDigit;
                  break;
               case DTMFDET_PARM_LAST_DURATION:
                  m_stDtmfDetSts.uLastDuration = pDtmfDetSts->uLastDuration;
                  break;
               case DTMFDET_PARM_LAST_TSTAMP:
                  m_stDtmfDetSts.uLastTimestamp = pDtmfDetSts->uLastTimestamp;
                  break;
               case DTMFDET_PARM_TSCALE:
                  m_stDtmfDetSts.uTimeScale = pDtmfDetSts->uTimeScale;
                  break;
               default:
                  break;
            }//switch
         }
      }//for
   }
   return 0;
}

int CRtpDtmf::GenDTMF(int nOpcode,int nDigit, int nDuration, int nPower)
      //first dtmf generate invoke by user api
      // ret -1: not enable
{
#if 0
      if(!m_stDtmfGenSts.bEnable)
      {
         return -1;
      }
#endif
   int nRes = 0;

   //int nDigit = AMP::HuDtmfCodeToDtmf2833((AMP::CHuDtmf::DTMF_CODE)nDtmfCode);
   //int nVolume    = 63 - (int)(0.5+((double)nPower * 63./100.));
   int nVolume    = nPower;//fixed 2012-06-12 setval with MGP origin val , not modified
   if(nDuration > MAX_DTMFGEN_DURATION)
      nDuration = MAX_DTMFGEN_DURATION;//if duration val exceed max limit duration
   //set enforce default value
   m_stDtmfGenSts.uTimestamp = m_pRtpComm->GetSendTimestamp();
   if(sRtpDtmfSender->Add(this, nDigit, nDuration, nVolume) == false) {
      nRes = -2;

      //m_stGenDtmfState.nVolume    = 63 - (int)(0.5+((double)nPower * 63./100.));;

      //m_stGenDtmfInfo.nMarker   = 1;


      //      gettimeofday(&m_stGenDtmfInfo.stGenStartTime, NULL); // for duration
   }
   return nRes;
}

//need SetDtmfGenInfParm() before call this func 
int CRtpDtmf::HandleGenDtmf(unsigned int nOpcode, DtmfGenInfo* pDtmfInfo, int nEndCnt, bool bForceEnd)
   //----
   // ret:
   //  0 : success
   // -1 : dtmf enable off
   // -2 : invalid opcode
   // -3 : not implement code
   // -4 : RTP Send fail
   // -5 : RtpComm is null
{
   if(nOpcode != OP_DTMF_GEN && nOpcode != OP_DTMF_GEN_2833)
      return -2;

   if(!m_stDtmfGenSts.bEnable) return -1;

   bool bEnd  = false;
   unsigned short usDuration;
   int nSendCnt = 1;
   switch((eDtmfOpcode)nOpcode)
   {
      case OP_DTMF_GEN://inband dtmf gen..
         return -3;
         break;
      case OP_DTMF_GEN_2833:
         {
            int nPayloadType;
            unsigned int uTimestamp;
            struct timeval stCurTime;

            gettimeofday(&stCurTime, NULL);
            unsigned int lCurTimeMS = stCurTime.tv_sec*1000 + stCurTime.tv_usec/1000;

            if(lCurTimeMS < pDtmfInfo->lStartTimeMS)
               usDuration = 20;//exception default val set
            else
               usDuration = lCurTimeMS - pDtmfInfo->lStartTimeMS;
            if(usDuration >= pDtmfInfo->usDuration || bForceEnd) {
               bEnd = true;
               usDuration = pDtmfInfo->usDuration;
            } //end decision at threadproc dtmfsender not here

            nPayloadType = m_stDtmfGenSts.ucPayloadType;
            uTimestamp   = m_stDtmfGenSts.uTimestamp;// + (usDuration) * 8;//(8000 / 1000);

            RtpPayloadRfc2833 rtpPayloadRfc2833;
            memset(&rtpPayloadRfc2833,0x00,sizeof(rtpPayloadRfc2833));
            RtpPayloadRfc2833 * pPayload = &rtpPayloadRfc2833;//(RtpPayloadRfc2833 *)(pPktData+sizeof(RTPHEADER));

            pPayload->event      = pDtmfInfo->usDigit;
            pPayload->edge       = bEnd|bForceEnd ? 1 : 0;
            pPayload->reserved   = 0;
            pPayload->volume     = pDtmfInfo->usVolume;
            pPayload->duration   = htons(usDuration * 8);//(8000/1000)); //Sample-rate / 1sec

            m_stDtmfGenSts.usSeq++;

            DtmfGenInfo dtmfGenInfo;
            //memset(&dtmfGenInfo,0x00,sizeof(dtmfGenInfo));
            if(bEnd)
            {
               dtmfGenInfo.usType = DT_RFC2833_END;//last gen dtmf
               nSendCnt = nEndCnt;
            }       
            else if(pDtmfInfo->bFirst)
            {
               dtmfGenInfo.usType = DT_RFC2833_START;//first gen dtmf
            }

            if(pDtmfInfo->bFirst){ dtmfGenInfo.usMarker = 1; pDtmfInfo->bFirst = false;}
            else dtmfGenInfo.usMarker = 0;

            dtmfGenInfo.usDigit = pPayload->event;
            dtmfGenInfo.usVolume = (unsigned short)(pPayload->volume* 1.58);//100 /63;
            dtmfGenInfo.usDuration = ntohs(pPayload->duration)/8;


            //send with sender
            int nSndLen=0;
            if(m_pRtpComm)
            {
               for(int i=0;i<nSendCnt;i++)
               {
                  if((nSndLen = m_pRtpComm->PutPayload_((unsigned char*)pPayload,sizeof(RtpPayloadRfc2833),
                              uTimestamp,dtmfGenInfo.usMarker, m_stDtmfGenSts.ucPayloadType))>=
                        RTP_HEADER_SIZE)
                  {
#ifdef DEBUG_DTMF
                     fprintf(stderr,"dtmf ts:%d m:%d pt:%d sndlen:%d ft:%lu cur:%lu\n",
                        uTimestamp,dtmfGenInfo.usMarker,m_stDtmfGenSts.ucPayloadType,nSndLen,
                           m_stDtmfGenSts.lFirstStartTimeMS,lCurTimeMS);
#endif
                     if(dtmfGenInfo.usMarker == 1) dtmfGenInfo.usMarker = 0;
                  }
               }//for
               if(pPayload->edge && m_RtpDtmfGenCallBack)
                  (*m_RtpDtmfGenCallBack)(m_pUserClass,&dtmfGenInfo);//user callback invoke
            }
            else return -5;

            //remember in memeber dtmf sts

            //
         }
         break;
      default:
         break;
   }
   return 0;
}

int CRtpDtmf::HandleDetDtmf2833(char *pRTPPayload, int nDataLen, RTPHEADER *pRtpHdr)
   //0 : success
   //give user flexible selection for handling dtmf
   //user check enable & payload value at user side and decision call this func or not
   //-1 : arg null
{
   if(!pRTPPayload || !pRtpHdr)
      return -1;

   RtpPayloadRfc2833 *pPayload =(RtpPayloadRfc2833*)pRTPPayload;

   bool bEnd = pPayload->edge==1?true:false;
   unsigned char ucDigit = pPayload->event;
   int nVolume = pPayload->volume;
   unsigned char usDuration = ntohs(pPayload->duration);

   if(m_stDtmfDetSts.uLastTimestamp != pRtpHdr->timestamp ||
         m_stDtmfDetSts.uLastDuration != usDuration ||
         m_stDtmfDetSts.ucLastDigit != ucDigit)
   {
      if(bEnd)
      {
         m_stDtmfDetSts.uLastTimestamp = pRtpHdr->timestamp;
         m_stDtmfDetSts.uLastDuration = usDuration;
         m_stDtmfDetSts.ucLastDigit = ucDigit;
         m_stDtmfDetSts.bFirst = false;
         DtmfDetInfo dtmfDetInfo;
         memset(&dtmfDetInfo,0x00,sizeof(dtmfDetInfo));
         dtmfDetInfo.usType =  DT_RFC2833_END;
         dtmfDetInfo.usDigit = ucDigit;
         dtmfDetInfo.usDuration = usDuration /8;
         dtmfDetInfo.usPower = nVolume * 100 /63;
         dtmfDetInfo.usRtpPayloadType = m_stDtmfDetSts.ucPayloadType;

         if(m_RtpDtmfNotifyCallBack)
            (*m_RtpDtmfNotifyCallBack)(m_pUserClass,&dtmfDetInfo);//user callback invoke
      }
      else
      {
         m_stDtmfDetSts.uLastTimestamp = 0;//this code designed for catch-dtmfevent that incomming with same timestamp
      }
#if 0
      else if(pRtpHdr->marker==1 || m_stDtmfDetSts.bFirst == false)
      {
         m_stDtmfDetSts.uLastTimestamp = pRtpHdr->timestamp;
         m_stDtmfDetSts.uLastDuration = usDuration;
         m_stDtmfDetSts.ucLastDigit = ucDigit;
         m_stDtmfDetSts.bFirst = true;
         DtmfDetInfo dtmfDetInfo;
         memset(&dtmfDetInfo,0x00,sizeof(dtmfDetInfo));
         dtmfDetInfo.usType =  DT_RFC2833_START;
         dtmfDetInfo.usDigit = ucDigit;
         dtmfDetInfo.usDuration = usDuration /8;
         dtmfDetInfo.usPower = nVolume * 100 /63;
         dtmfDetInfo.usRtpPayloadType = m_stDtmfDetSts.ucPayloadType;

         if(m_RtpDtmfNotifyCallBack)
            (*m_RtpDtmfNotifyCallBack)(m_pUserClass,&dtmfDetInfo);//user callback invoke
      }
#endif
   }

   return 0;
}
/*inline CHuDtmf::DTMF_CODE Dtmf2833ToHuDtmfCode(RtpPayloadRfc2833::DTMF_DIGIT nDtmfRfc2833)
{
   switch (nDtmfRfc2833) 
   {
      case RtpPayloadRfc2833::DTMFDIGIT0  : return CHuDtmf::DTMF_0;
      case RtpPayloadRfc2833::DTMFDIGIT1  : return CHuDtmf::DTMF_1;
      case RtpPayloadRfc2833::DTMFDIGIT2  : return CHuDtmf::DTMF_2;
      case RtpPayloadRfc2833::DTMFDIGIT3  : return CHuDtmf::DTMF_3;
      case RtpPayloadRfc2833::DTMFDIGIT4  : return CHuDtmf::DTMF_4;
      case RtpPayloadRfc2833::DTMFDIGIT5  : return CHuDtmf::DTMF_5;
      case RtpPayloadRfc2833::DTMFDIGIT6  : return CHuDtmf::DTMF_6;
      case RtpPayloadRfc2833::DTMFDIGIT7  : return CHuDtmf::DTMF_7;
      case RtpPayloadRfc2833::DTMFDIGIT8  : return CHuDtmf::DTMF_8;
      case RtpPayloadRfc2833::DTMFDIGIT9  : return CHuDtmf::DTMF_9;
      case RtpPayloadRfc2833::DTMFDIGITS  : return CHuDtmf::DTMF_STAR;
      case RtpPayloadRfc2833::DTMFDIGITP  : return CHuDtmf::DTMF_PND;
      case RtpPayloadRfc2833::DTMFDIGITA  : return CHuDtmf::DTMF_A;
      case RtpPayloadRfc2833::DTMFDIGITB  : return CHuDtmf::DTMF_B;
      case RtpPayloadRfc2833::DTMFDIGITC  : return CHuDtmf::DTMF_C;
      case RtpPayloadRfc2833::DTMFDIGITD  : return CHuDtmf::DTMF_D;
      default: break;
   }
 
   return CHuDtmf::TONE_END;
}
 
inline RtpPayloadRfc2833::DTMF_DIGIT HuDtmfCodeToDtmf2833(CHuDtmf::DTMF_CODE nDtmfCode)
{
   switch (nDtmfCode) 
   {
      case CHuDtmf::DTMF_0   :   return RtpPayloadRfc2833::DTMFDIGIT0 ;
      case CHuDtmf::DTMF_1   :   return RtpPayloadRfc2833::DTMFDIGIT1 ;
      case CHuDtmf::DTMF_2   :   return RtpPayloadRfc2833::DTMFDIGIT2 ;
      case CHuDtmf::DTMF_3   :   return RtpPayloadRfc2833::DTMFDIGIT3 ;
      case CHuDtmf::DTMF_4   :   return RtpPayloadRfc2833::DTMFDIGIT4 ;
      case CHuDtmf::DTMF_5   :   return RtpPayloadRfc2833::DTMFDIGIT5 ;
      case CHuDtmf::DTMF_6   :   return RtpPayloadRfc2833::DTMFDIGIT6 ;
      case CHuDtmf::DTMF_7   :   return RtpPayloadRfc2833::DTMFDIGIT7 ;
      case CHuDtmf::DTMF_8   :   return RtpPayloadRfc2833::DTMFDIGIT8 ;
      case CHuDtmf::DTMF_9   :   return RtpPayloadRfc2833::DTMFDIGIT9 ;
      case CHuDtmf::DTMF_STAR:   return RtpPayloadRfc2833::DTMFDIGITS ;
      case CHuDtmf::DTMF_PND :   return RtpPayloadRfc2833::DTMFDIGITP ;
      case CHuDtmf::DTMF_A   :   return RtpPayloadRfc2833::DTMFDIGITA ;
      case CHuDtmf::DTMF_B   :   return RtpPayloadRfc2833::DTMFDIGITB ;
      case CHuDtmf::DTMF_C   :   return RtpPayloadRfc2833::DTMFDIGITC ;
      case CHuDtmf::DTMF_D   :   return RtpPayloadRfc2833::DTMFDIGITD ;
      default: break;
   }
 
   return RtpPayloadRfc2833::DTMFDIGITN;
}*/
 

//bool bRtpDtmfSenderInit = CRtpDtmfSender::Init();


static Singleton_Destroyer<AMT::CRtpDtmfSender> CRtpDtmfSender_Destroyer;
CRtpDtmfSender *CRtpDtmfSender::m_pInstance = NULL;

CRtpDtmfSender* CRtpDtmfSender::GetInstance()
{
   if(m_pInstance == NULL)
   {
      m_pInstance = new CRtpDtmfSender;
      CRtpDtmfSender_Destroyer.Register(m_pInstance);
      m_pInstance->Init();//default Init Interval 50, endpktcnt 3
   }
   return m_pInstance;
}
bool CRtpDtmfSender::Init(int nIntervalMS, int nEndPktCnt)
{
   m_nInterval = nIntervalMS;
   m_nMaxEndPkt = nEndPktCnt;
   return true;
}

CRtpDtmfSender::CRtpDtmfSender() 
{
   Init();//default dtmf gen interval(50ms) endpktcnt(3) initializing
   Start();
}

CRtpDtmfSender::~CRtpDtmfSender() 
{
   Stop();
}


// GenDtmf
bool CRtpDtmfSender::Add(CRtpDtmf* pRtpDtmf,
      int nDigit, int nDuration, int nVolume)
{
   if(!pRtpDtmf)
      return false;

   std::map <CRtpDtmf*, DtmfGenInfo>::iterator it;
   DtmfGenInfo dtmfGenInfo;
   DtmfGenInfo * pDtmfInfo = &dtmfGenInfo;
   //memset(pDtmfInfo,0x00,sizeof(dtmfGenInfo));
   pDtmfInfo->usDigit    = (unsigned short)nDigit;
   pDtmfInfo->usDuration = (unsigned short)nDuration;
   pDtmfInfo->usVolume   = (unsigned short)nVolume;
   pDtmfInfo->bFirst = true;
   timeval tval;
   gettimeofday(&tval, NULL);
   pDtmfInfo->lCurTimeMS= pDtmfInfo->lStartTimeMS = tval.tv_sec *1000 + tval.tv_usec/1000;
   pDtmfInfo->lEndTimeMS = pDtmfInfo->lCurTimeMS + pDtmfInfo->usDuration;

   pDtmfInfo->usMarker   = 1;
   pDtmfInfo->usEndCnt   = 0;

#ifdef DEBUG_DTMF
   fprintf(stderr,"add digit %d dur %d vol %d curms:%lu endms:%lu\n",
      pDtmfInfo->usDigit,pDtmfInfo->usDuration,pDtmfInfo->usVolume,
         pDtmfInfo->lCurTimeMS,pDtmfInfo->lEndTimeMS); 
#endif
   m_Lock.Lock();
   if(!m_Map.empty()) it = m_Map.find(pRtpDtmf);
   else it = m_Map.end();
   if(it == m_Map.end()) 
   {
      m_Map[pRtpDtmf] = dtmfGenInfo;
   } 
   else 
   {
      DtmfGenInfo *pDtmfInfo2   = &it->second;
      if(pDtmfInfo2 && (pRtpDtmf->HandleGenDtmf(OP_DTMF_GEN_2833,pDtmfInfo2,m_nMaxEndPkt, true))!=0)
      {
#ifdef DEBUG_DTMF
         fprintf(stderr,"HandleGenDtmf(last) fail\n");
#endif
      }
      m_Map.erase(it);//this clause not need ,because below clause do substitude key&val, but do for ensure substitude
      m_Map[pRtpDtmf] = dtmfGenInfo;
   }
   m_Lock.Unlock();
   return true;
}



void * CRtpDtmfSender::ThreadProc()
   //ret -1: HandlGenDtmf err
{
   MapDtmfGenInfo::iterator itmap;
   CRtpDtmf * pRtpDtmf = NULL;
   DtmfGenInfo * pDtmfInfo = NULL;
   timeval curTimeVal, prevTimeVal;
   unsigned int lCurTimeMS,lPrevTimeMS;
   gettimeofday(&prevTimeVal, NULL);
   lPrevTimeMS = prevTimeVal.tv_sec *1000 + prevTimeVal.tv_usec/1000;

   int nInterval;
   int nRet=0;

   while (!DoExit()) 
   {
      m_Lock.Lock();
      if(m_Map.size() > 0) 
      {
         gettimeofday(&curTimeVal, NULL);
         lCurTimeMS = curTimeVal.tv_sec *1000 + curTimeVal.tv_usec/1000;
         nInterval = abs((int)(lCurTimeMS - lPrevTimeMS));
         if(nInterval >= m_nInterval)
         {
            std::list<MapDtmfGenInfo::iterator> lsRtpDtmf;
            std::list<MapDtmfGenInfo::iterator>::iterator itls;
            lPrevTimeMS = lCurTimeMS;

            for(itmap = m_Map.begin(); itmap != m_Map.end(); itmap++)
            {
               pRtpDtmf     = (CRtpDtmf *)itmap->first;
               pDtmfInfo = (DtmfGenInfo*)&(itmap->second);
               if(!pRtpDtmf || !pDtmfInfo || pDtmfInfo->lEndTimeMS==0) continue;

               if(pDtmfInfo->lEndTimeMS <= lCurTimeMS)
               {
                  if((nRet=pRtpDtmf->HandleGenDtmf(OP_DTMF_GEN_2833,pDtmfInfo,m_nMaxEndPkt, true))!=0)
                  {
#ifdef DEBUG_DTMF
                     fprintf(stderr,"HandleGenDtmf(last) fail ret=%d\n",nRet);
#endif
                  }
                  lsRtpDtmf.push_back(itmap);
               }
               else
               {
                  if((nRet=pRtpDtmf->HandleGenDtmf(OP_DTMF_GEN_2833,pDtmfInfo,m_nMaxEndPkt, false))!=0)
                  {
#ifdef DEBUG_DTMF
                     fprintf(stderr,"HandleGenDtmf fail ret=%d\n",nRet);
#endif
                  }
               }
            }//for

            if(!lsRtpDtmf.empty())
            {
               for(itls=lsRtpDtmf.begin();itls != lsRtpDtmf.end();itls++)
               {
                  itmap = *itls;
                  m_Map.erase(itmap);
               }
               lsRtpDtmf.clear();
            }
         }
      }
      m_Lock.Unlock();
      usleep(1000);//1ms
   }
   return NULL;
}

}; //namespace



