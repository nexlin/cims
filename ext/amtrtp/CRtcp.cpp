#include "CRtcp.h"

// C++ 
#ifndef NULL
#define NULL 0
#endif


namespace AMT 
{
static void timersubx(struct timeval *tvend, struct timeval *tvstart, struct timeval *tvdiff)
{
   tvdiff->tv_sec = tvend->tv_sec - tvstart->tv_sec;
   tvdiff->tv_usec = tvend->tv_usec - tvstart->tv_usec;
   if (tvdiff->tv_usec < 0) 
   {
      tvdiff->tv_sec --;
      tvdiff->tv_usec += 1000000;
   }
}

void CRtcpCtx::Reset(){
   m_TotalRxPackets=0;
   m_TotalRxBytes  =0;
   m_TotalRxLoss   =0;

   m_LastTxSeq     =0;
   m_LastTxTS      =0;
   m_TotalTxPackets=0;
   m_TotalTxBytes  =0;

   m_LastRxSSRC    =0;
   m_LastRxTS      =0;

   m_RxFirstRTP.tv_sec=0;
   m_RxFirstRTP.tv_usec=0;
   m_FirstTS=0;

   m_RxLSRTime.tv_sec=0;
   m_RxLSRTime.tv_usec=0;


   m_Received      =0;
   m_ExpPrior      =0;
   m_RxPrior       =0;
   m_Transit       =0;
   m_Jitter        =0;
   m_RoundTrip     =0.0;

   m_SeqCtrl.Reset();
   memset(&m_LastSR,0x00,sizeof(m_LastSR));
   memset(&m_LastRR,0x00,sizeof(m_LastRR));
}
void CRtcpCtx::SeqRestarted(void){
   m_RxFirstRTP.tv_sec=0;
   m_RxFirstRTP.tv_usec=0;
   m_FirstTS=0;
   m_RxLSRTime.tv_sec=0;
   m_RxLSRTime.tv_usec=0;

   m_LastTxSeq =0;

   m_Received = 0;
   m_ExpPrior = 0;
   m_RxPrior  = 0;
   m_Transit  = 0;
   m_Jitter   = 0;
   m_RoundTrip=0.0;
   memset(&m_LastSR,0x00,sizeof(m_LastSR));
   memset(&m_LastRR,0x00,sizeof(m_LastRR));
}
void CRtcpCtx::RTPReceived(unsigned int seq, unsigned int ts, 
                           unsigned int ssrc, unsigned int len_payload,
                           unsigned int nclock_rate,RtpChkStatus_T* oseq_st){
   struct timeval now;
   RtpChkStatus_T seq_st;
   unsigned int last_seq;
   unsigned int ntp_msw,ntp_lsw;

   gettimeofday(&now,NULL);
   Timeval2NTPTime(&now,&ntp_msw,&ntp_lsw);

   if(m_TotalRxPackets==0)
   {
      m_SeqCtrl.Init(seq);
      m_RxFirstRTP.tv_sec=0;
   }

   m_TotalRxPackets++;
   m_TotalRxBytes += len_payload;
   m_LastRxSSRC = ssrc;

   last_seq = m_SeqCtrl.max_seq;
   m_SeqCtrl.Update((unsigned short)seq,&seq_st);

   oseq_st->status.value = seq_st.status.value;
   oseq_st->diff = seq_st.diff;

   if(seq_st.status.flag.restart){
      SeqRestarted();
      m_RxFirstRTP.tv_sec=0;
   
   }
   //duplicated
   if(seq_st.status.flag.dup){
      return; 
   }

   if(seq_st.status.flag.outorder && !seq_st.status.flag.probation){
      // out of order packet
   }

   //bad packet
   if(seq_st.status.flag.bad){
      return;
   }

   m_Received++;

   if( seq_st.diff >1 ){
      m_TotalRxLoss+=(seq_st.diff-1);
   }

   // compute jitter
   int nlocal_ts = 0;
   if( m_RxFirstRTP.tv_sec == 0 ){
      gettimeofday(&m_RxFirstRTP,NULL);
      m_Transit=0;
      m_Jitter =0;
      m_FirstTS=ts;
   }else{
      nlocal_ts = GetTimeDiff(&m_RxFirstRTP, NULL);
      if ( nclock_rate <= 0 ) {
         nclock_rate = 8 ; //default
      }else{
         nclock_rate /=(1000);
      }
      nlocal_ts *= nclock_rate;
      int transit = (int)((nlocal_ts+m_FirstTS) - ts);
      if( m_Transit == 0 ){
         m_Transit = transit;
      }
      int d = (int)(transit - m_Transit);
      m_Transit = transit;
      if (d < 0) d = -d;
      /* calculation scaled to reduce round off error */
      m_Jitter += d - ((m_Jitter + 8) >> 4);
     
      //MAX 30sec 
      if( (m_Jitter>>4) >= 30000){
         m_RxFirstRTP.tv_sec=0;
      }
   }
}

void CRtcpCtx::RTPSent(unsigned int seq,unsigned int ts,unsigned int len_payload){
   m_LastTxSeq = seq;
   m_TotalTxPackets ++;
   m_TotalTxBytes += len_payload;
   m_LastTxTS = ts;
}

int CRtcpCtx::BuildSR(char *packet,unsigned int myssrc)
{
   RTCPPacketHeader_T*H;
   RTCPSReport_T*sr;

   struct timeval now;
   unsigned int ntp_msw,ntp_lsw;

   H = (RTCPPacketHeader_T*)packet;
   H->version = 2;
   H->p       = 0;
   H->count   = 0;
   H->pt      = RT_SENDER_REPORT;
   H->length  = 6; // ssrc(1) + sr(5)
   H->ssrc = myssrc;
   H->hton();

   gettimeofday(&now,NULL);
   Timeval2NTPTime(&now,&ntp_msw,&ntp_lsw);

   sr = (RTCPSReport_T*)(packet+8);    
   sr->ntp_sec = ntp_msw; 
   sr->ntp_frac= ntp_lsw;
   sr->rtp_ts = m_LastTxTS;
   sr->sender_pcount = m_TotalTxPackets;
   sr->sender_bcount = m_TotalTxBytes;

   sr->hton();
   return 28; //header(4) + ssrc(4) + sr(20)
}

int CRtcpCtx::BuildSDES(char *packet,unsigned int myssrc, char* cname)
{
   RTCPPacketHeader_T*H;
   RTCPSDES_T*sdes;

   int rc=0;

   int value = (1+1+strlen(cname)) / 4; // type + length + value
   int rest= (1+1+strlen(cname)) % 4; // rounds 
   if ( rest != 0 ){
      if( rest == 1 ){
         strcat(cname,"   ");
      }else if( rest == 2 ){
         strcat(cname,"  ");
      }else if( rest == 3 ){
         strcat(cname," ");
      }
      value += 1;
   }

   H = (RTCPPacketHeader_T*)packet;
   H->version = 2;
   H->p       = 0;
   H->count   = 1;
   H->pt      = RT_SOURCE_DESC;
   H->length  = 1+value; // ssrc(1) + sdes
   H->ssrc = myssrc;
   H->hton();

   sdes = (RTCPSDES_T*)(packet+8);
   sdes->type = RT_SDES_CNAME;   //1byte
   sdes->length = strlen(cname); //1byte
   memcpy(packet+10, cname, sdes->length);

   rc = (1+1+value)*4; // header+ssrc+sdes

   return rc;
}

int CRtcpCtx::BuildRR(char*packet,unsigned int myssrc)
{
   RTCPPacketHeader_T*H;
   RTCPRReport_T*rr;

   unsigned int last_seq;
   unsigned int expected,expected_interval,received_interval;
   unsigned int lost_interval,lost_fract;

   // Latest sequence number
   last_seq  =(m_SeqCtrl.cycles & 0xFFFF0000L);
   last_seq += m_SeqCtrl.max_seq ;

       
   //exception. not yet rtp received. we don't notify RR
   if( m_TotalRxPackets == 0 ||
       last_seq == 0 ){
      return 0;
   }

   H = (RTCPPacketHeader_T*)packet;
   H->version = 2;
   H->p       = 0;
   H->count   = 1;
   H->pt      = RT_RECEIVER_REPORT;
   H->length  = 7; // ssrc(1) + rr(6)
   H->ssrc = myssrc;
   H->hton();

   rr = (RTCPRReport_T*)(packet+8);
   rr->ssrc   = m_LastRxSSRC;

   // cululative lost 
   rr->total_lost_2 = (m_TotalRxLoss >> 16) & 0xFF;
   rr->total_lost_1 = (m_TotalRxLoss >> 8) & 0xFF;
   rr->total_lost_0 = (m_TotalRxLoss & 0xFF);

   // fraction lost
   expected          = last_seq - m_SeqCtrl.base_seq + 1;

   expected_interval = expected - m_ExpPrior;
   m_ExpPrior        = expected; 
   received_interval = m_TotalRxPackets - m_RxPrior;
   m_RxPrior         = m_TotalRxPackets;

   if(expected_interval >= received_interval){
      lost_interval = expected_interval - received_interval;
   }else{ 
      lost_interval = 0 ;
   }

   if(expected_interval==0 || lost_interval == 0){
      lost_fract = 0;
   }else{
      lost_fract = (lost_interval << 8) / expected_interval;
   }
   rr->fract_lost = lost_fract ;
   rr->last_seq = last_seq ;

   // jitter set
   rr->jitter = (m_Jitter >> 4);

   // LSR, DLSR set
   if(m_RxLSRTime.tv_sec==0 || m_ThemLSR==0){
      rr->lsr = 0 ;
      rr->dlsr= 0 ;
   }else{
        struct timeval now,dlsr ;
        gettimeofday(&now,NULL);
        timersubx(&now,&m_RxLSRTime,&dlsr);
        rr->lsr = (m_ThemLSR) ;
        rr->dlsr= (((dlsr.tv_sec*1000)+(dlsr.tv_usec/1000))*65536)/1000;
   }

   rr->hton();

   return 32;
}
void CRtcpCtx::RTCPReceived(unsigned char *pData,unsigned int nSize)
{
   int nErroNum=0;
   char szErrStr[128]; szErrStr[0]=0x00;
   //rtcp packet min size is 4
   if ( pData == NULL || nSize < 4  ) {
      sprintf(szErrStr,"the rtcp packet size too short. size(%d)\n",nSize);
      return;
   }
   unsigned char* pEnd= pData + nSize;
   unsigned char* pCur= pData;
   RTCPPacketHeader_T *pHdr=NULL;
   unsigned int uiSizeofPadding=0;

   int nCurSize=0;
   unsigned int uiRealLen=0;
   int nPacketCount=0;
   //while check
   while(pCur < pEnd){

      nPacketCount++;
      nErroNum=0;
      //min size check
      nCurSize=(pEnd - pCur);
      if( nCurSize < 5 ){
         nErroNum=1;//size error
         sprintf(szErrStr,"%d chunk. rtcp packet size error cur:%d/5\n",nPacketCount,nCurSize);
         break;
      }
      
      //mapping rtcp header
      pHdr=(RTCPPacketHeader_T*)pCur;
      pHdr->ntoh();

      //check rtcp version
      if( pHdr->version !=2 ){
         nErroNum=2;
         sprintf(szErrStr,"%d chunk. rtcp version error ver:%d\n",nPacketCount,pHdr->version);
         break;
      }

      //check padding bit
      uiSizeofPadding=0; 
      if (pHdr->p){
         uiSizeofPadding=*(pEnd-1);
         if ( (int)(nCurSize - uiSizeofPadding) <= 0 ){
            nErroNum=4; //padding size error 2
            sprintf(szErrStr,"%d chunk. rtcp padding size error cur:%d/4\n",nPacketCount,(nCurSize - uiSizeofPadding));
            break;
         }
         pEnd=pData + nSize - uiSizeofPadding;
      }
      //4byte단위. header(4byte)를 제외한 body
      uiRealLen=(pHdr->length)*4 - uiSizeofPadding;

      //check header length..rtcp header length  body size
      if ( (unsigned int)(pEnd - pCur) < uiRealLen ||
           uiRealLen < 4 ||
           uiRealLen > 1472 ){ //MTU size Check
         nErroNum=5; //header size error!
         sprintf(szErrStr,"%d chunk. rtcp header size error cur:%d/header:%d\n",nPacketCount,(int)(pEnd - pCur),uiRealLen);
         break;
      }
      //check header type
      if( pHdr->pt < RT_SENDER_REPORT ||  pHdr->pt > RT_APP_DEFINED ){
         nErroNum=6; //pt error!
         sprintf(szErrStr,"%d chunk. unsupported Rtcp Type:%d\n",nPacketCount,pHdr->pt);
         break;
      }
      //one packet parsing
      ProcessRTCPPacket(pHdr,pCur+4,uiRealLen);
      pCur += uiRealLen + 4;
   }//end while
}

void CRtcpCtx::ProcessRTCPPacket(RTCPPacketHeader_T* pHdr, unsigned char* pBody, unsigned int uiLen)
{
   //skip SSRC, 4byte
   if( uiLen < 4 ) {
      return;
   }
   //check ssrc
   uiLen -=4 ;
   if( uiLen <= 0 ) return;
   pBody +=4 ;


   RTCPSReport_T* sr=NULL;
   RTCPRReport_T* rr=NULL; 
   int nRCCount=0;
   struct timeval now ;
   unsigned int  dlsr, lsr, msw, lsw, comp;
   double        rttsec=0;
   unsigned long rtt=0;
   switch(pHdr->pt){
   case RT_SENDER_REPORT:{
      if( uiLen < 20 ){
         return;
      }
      //memcpy last sr copy
      memcpy(&m_LastSR,pBody,sizeof(m_LastSR));
      //sr mapping
      sr = (RTCPSReport_T*)(pBody);
      //received packet.
      sr->ntoh();
      gettimeofday(&m_RxLSRTime,NULL);
      m_ThemLSR = ((sr->ntp_sec & 0x0000ffff) << 16) |
                  ((sr->ntp_frac& 0xffff0000) >> 16);
      uiLen -= 20;
      pBody += 20;
      nRCCount=pHdr->count;
      if( uiLen <= 0 || nRCCount <= 0 ){
         return;
      }
      //check receive report min size
      if( uiLen < 24 ) return;
   }
   case RT_RECEIVER_REPORT:{
      //the only RR
      if( pHdr->pt == RT_RECEIVER_REPORT ){
         nRCCount=pHdr->count;
      }
      if( uiLen < 24 || nRCCount <=  0 ){
         return;
      }
      //only the first nRCCount.............. 
      memcpy(&m_LastRR,pBody,sizeof(m_LastRR));
      //sr mapping
      rr = (RTCPRReport_T*)(pBody);
      rr->ntoh();
      // rr processing
      gettimeofday(&now,NULL);
      Timeval2NTPTime(&now,&msw,&lsw);
      if(rr->lsr && rr->dlsr){
         comp = ((msw & 0xffff) << 16) | ((lsw & 0xffff0000) >> 16);
         lsr  = rr->lsr ;
         dlsr = rr->dlsr;
         rtt  = comp - lsr - dlsr ;
         if(rtt< 4294){
            rtt = (rtt * 1000000) >> 16;
         }else{
            rtt = (rtt * 1000) >> 16;
            rtt *= 1000;
         } 

         rtt =(unsigned int)( rtt/1000.) ;
         rttsec = rtt/1000.;
         m_RoundTrip  = rttsec ;
      }
   }
   break;

   case RT_SOURCE_DESC:
   case RT_GOOD_BYE:
   case RT_APP_DEFINED:
   //do nothing
   break;

   }
}

// return  < 0 : error
//         = 0 : error
//         > 0 : packetizing size (bytes)
int CRtcpCtx::BuildRTCPPacket(char*packet, unsigned int packetlen, unsigned int myssrc, int mode, char* cname)
{ 
   if(packet==NULL || packetlen <= RTCP_MIN_BUFF ){
      return -1;
   }

   int rc = 0;
   char szcname[256];
   memset(szcname, 0x00, sizeof(szcname));

   if( mode == SR_SDES || mode == RR_SDES ){
     if( cname == NULL || cname[0]==0x00 ){
        return -1;
     }
#ifndef WIN32
     snprintf(szcname,sizeof(szcname)-10,"%u@%s",myssrc,cname);
#else
	 sprintf(szcname,"%u@%s",myssrc,cname);
#endif
   }

   switch(mode)
   {
      case SR_ONLY:
         rc=BuildSR(packet,myssrc);
         break;
      case SR_SDES:
         rc=BuildSR(packet,myssrc);
         if( rc <= 0 ) break;
         rc+=BuildSDES(packet+28,myssrc,szcname);
         break;
      case RR_ONLY:
         rc=BuildRR(packet,myssrc);
         break;
      case RR_SDES:
         rc=BuildRR(packet,myssrc);
         if( rc <= 0 ) break;
         rc+=BuildSDES(packet+32,myssrc,szcname);
         break;
      default:
         // error
      break;
   }
   return rc;
}

//convert ntp sec to timeval.tv_sec
#define JAN_1970  0x83aa7e80
//timeval.tv_usec => ntp usec
#define NTPFRAC(x) (4294 * (x) + ((1981 * (x))>>11))
//ntp usec => timeval.tv_usec
#define USEC(x) (((x) >> 12) - 759 * ((((x) >> 10) + 32768) >> 16)) 

void CRtcpCtx::Timeval2NTPTime(struct timeval *p_tv, unsigned int *p_msw, unsigned int *p_lsw)
{
   //check param
   if( p_tv == NULL || p_msw == NULL || p_lsw == NULL ){
      return;
   }
   *p_msw=0;
   *p_lsw=0;
   *p_msw = (unsigned int)(p_tv->tv_sec + JAN_1970 ); /* Sec between 1900 and 1970 */
   *p_lsw = (unsigned int)(NTPFRAC(p_tv->tv_usec));
}

void CRtcpCtx::NTPTime2Timeval(struct timeval *p_tv, unsigned int msw, unsigned int lsw)
{
   //check param
   if( p_tv == NULL ){
      return;
   }
   p_tv->tv_sec=0;
   p_tv->tv_usec=0;

   p_tv->tv_sec = (msw - JAN_1970);
   p_tv->tv_usec = USEC(lsw);
}
unsigned int CRtcpCtx::GetTimeDiff(struct timeval *p_tvstart, struct timeval *p_tvend){
   unsigned int ui_tt=0;
   unsigned int ui_dsec=0;
   unsigned int ui_dusec=0;

   struct timeval curtime;
   //check param
   if( p_tvstart == NULL ) return 0;
   if( p_tvend == NULL ){
      memset(&curtime,0x00,sizeof(curtime));
      gettimeofday(&curtime,NULL);
      p_tvend=&curtime;
   }
 
   //get diff sec 
   ui_dsec = p_tvend->tv_sec - p_tvstart->tv_sec;
   //get diff usec 
   if ( p_tvend->tv_usec >= p_tvstart->tv_usec ){
      ui_dusec= p_tvend->tv_usec - p_tvstart->tv_usec;
   }else{
      ui_dusec = (p_tvend->tv_usec + 1000000) - p_tvstart->tv_usec;
      ui_dsec--;
   }
   //change msec
   ui_tt = (ui_dsec*1000) + (ui_dusec/1000);
   return ui_tt;
}


}; // namespace AMT
