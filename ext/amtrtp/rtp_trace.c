#include "rtp_trace.h"

int make_rtp_log (char * pRtpPkt, unsigned int unRtpPktLen,
                      char * pszLog, unsigned int unLogBufLen)
{
  char evtname;
  unsigned char evtvalue;
  unsigned int unRtpOffset = 0;
  unsigned int unLogOffset = 0;
  unsigned int payloadtypevalue, i;
  char *payloadtypename = "UNKNOWN";

  if (pszLog == NULL || unLogBufLen < RTP_BUF_MINIMUM)
    return -1;
  
  if (pRtpPkt == NULL || unRtpPktLen < 12) {
    sprintf(pszLog + unLogOffset, 
       DIV_LINE "*** Data Length Too Short ***\n");
    return -2;
  }

  ////////// RTP Header //////////
  RTPPacketHeader_T *rtpH = (RTPPacketHeader_T*)pRtpPkt;
  rtpH->seqnumber = ntohs(rtpH->seqnumber); 
  rtpH->timestamp = ntohl(rtpH->timestamp); 
  rtpH->ssrc      = ntohl(rtpH->ssrc); 
  unRtpOffset = 12;

  if (rtpH->payloadtype== PT_AMR) {
    payloadtypename = "AMR" ;
  }
  else if (rtpH->payloadtype == PT_DTMF) {
    payloadtypename = "DTMF" ;
  }

  sprintf( pszLog + unLogOffset,
      DIV_LINE
      "RTP Header information\n"
      "Version         : %d\n"
      "Padding         : %d\n"
      "Extension       : %d\n"
      "CSRC count      : %d\n"
      "Marker          : %d\n"
      "PayloadType     : %d(%s)\n"
      "Sequence Number : %d\n"
      "Timestamp       : %u\n"
      "SSRC            : %u\n",
      rtpH->version,
      rtpH->padding,
      rtpH->extension,
      rtpH->cc,
      rtpH->marker,
      rtpH->payloadtype, payloadtypename,
      rtpH->seqnumber, 
      rtpH->timestamp,
      rtpH->ssrc
  );
  unLogOffset = strlen(pszLog); 

  ////////// RTP Header CSRC //////////
  for (i = 0; i < rtpH->cc; ++i) {

    if (i < 4 && unLogBufLen > unLogOffset + LINE_LENGTH*3 ) {
      sprintf( pszLog + unLogOffset, 
          "CSRC[%02d]        : %u\n", i, ntohl(rtpH->csrc[i]) );   
      unRtpOffset += 4;
      unLogOffset = strlen(pszLog); 
    }
    else if (i == 4) {
      sprintf(pszLog + unLogOffset, "...\n", i);   
      unRtpOffset += 4;
      unLogOffset += 4; 
    }
    else 
      unRtpOffset += 4;
  } 
  sprintf( pszLog + unLogOffset, DIV_LINE );
  unLogOffset += LINE_LENGTH;

  ////////// DTMF Payload Header //////////
  if (payloadtypevalue==101 && unRtpPktLen >= (unRtpOffset+4)) {

    RTPPayloadDTMF_T *dtmfPH = (RTPPayloadDTMF_T *)&pRtpPkt[unRtpOffset];
    dtmfPH->duration = ntohs(dtmfPH->duration);
    unRtpOffset += 4;

    evtvalue = dtmfPH->event;
    if (evtvalue==0){ // digit
      evtname='0';
    } else if ((evtvalue>=1)&&(evtvalue <= 9)) {
      evtname=((int)'9' - ( 9 - evtvalue));
    } else if (evtvalue==10) { // start *
      evtname='*';
    } else if (evtvalue==11) { // pound #
      evtname='#';
    } else {
      evtname=' ';
    }

    if (unLogBufLen > unLogOffset + LINE_LENGTH*2) {
      sprintf(pszLog + unLogOffset,  
          "Payload Header information\n"
          "Event   : %d(%c)\n"
          "Edge    : %d\n"
          "Reserved: %d\n"
          "Volume  : %d\n"
          "Duration: %d\n"
          DIV_LINE, 
          evtvalue, evtname, 
          dtmfPH->edge,
          dtmfPH->reserved,
          dtmfPH->volume,
          dtmfPH->duration
      );
      unLogOffset = strlen(pszLog); 
    }
  }

  ////////// RTP Hex Dumping //////////
  for (unRtpOffset = 0; unLogBufLen > unLogOffset + LINE_LENGTH*2; unLogOffset += LINE_LENGTH) {

    if (unRtpPktLen >= unRtpOffset+16) {
      sprintf(pszLog + unLogOffset, 
          "%02x %02x %02x %02x "
          "%02x %02x %02x %02x "
          "%02x %02x %02x %02x "
          "%02x %02x %02x %02x\n",
          pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], 
          pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], 
          pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], 
          pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++], pRtpPkt[unRtpOffset++] );
    }
    else {

      if (unRtpOffset > 0 ) --unRtpOffset;
      for( ; unRtpPktLen > unRtpOffset; unLogOffset += 3) {
        sprintf(pszLog + unLogOffset,"%02x ", pRtpPkt[unRtpOffset++]); 
      }
      break;
    }
  }  

  ////////// END LINE //////////
  if (unRtpPktLen > unRtpOffset) { 
    sprintf(pszLog + unLogOffset, "%s" LAST_DIV_LINE,
              pszLog[unLogOffset-1]=='\n' ? "" : "\n" );
  }
  else { 
    sprintf(pszLog + unLogOffset,"%s" DIV_LINE, 
              pszLog[unLogOffset-1]=='\n' ? "" : "\n" );
  }

  return 0;
}

int make_rtcp_log (char *pRtcpPkt, unsigned int unRtcpPktLen,
                    char *pszLog, unsigned int unLogBufLen)

{
  unsigned char evtvalue;
  int ret = 0;
  unsigned int unRcount ;
  unsigned int unRtcpOffset = 0;
  unsigned int unLogOffset = 0;
  unsigned int payloadtypevalue, i;
  char *payloadtypename = "UNKNOWN";

  if (pszLog == NULL || unLogBufLen < RTCP_BUF_MINIMUM)
    return -1;
  
  if (pRtcpPkt == NULL || unRtcpPktLen < 8) {
    sprintf(pszLog + unLogOffset,
       DIV_LINE "*** Data Length Too Short ***\n" );
    return -2;
  }

  ////////// RTCP Common Header ////////// 
  RTCPPacketHeader_T *rtcpH = (RTCPPacketHeader_T*)pRtcpPkt;
  rtcpH->length = ntohs(rtcpH->length);
  rtcpH->ssrc   = ntohl(rtcpH->ssrc);
  unRtcpOffset += 8;

  unRcount = rtcpH->count;
  payloadtypevalue = rtcpH->pt; 

  if (payloadtypevalue == 200) 
    payloadtypename = "Sender Report";
  else if (payloadtypevalue == 201) 
    payloadtypename = "Recveiver Report"; 

  sprintf( pszLog + unLogOffset,
      DIV_LINE
      "RCTP Header information\n"
      "Version     : %d\n"
      "Padding     : %d\n"
      "ReportCount : %d\n"
      "PayloadType : %d(%s)\n"
      "Length      : %d\n"
      "SSRC        : %u\n",
      rtcpH->version,
      rtcpH->p,
      unRcount,
      payloadtypevalue, payloadtypename,
      rtcpH->length,
      rtcpH->ssrc
  );
  unLogOffset = strlen(pszLog); 

  ////////// SR, RR //////////
  if (payloadtypevalue == 200) { 
    ret = _format_rtcp_sr(&pRtcpPkt[unRtcpOffset], unRtcpPktLen - unRtcpOffset, 
              &pszLog[unLogOffset], unLogBufLen - unLogOffset, unRcount);
  }
  else if (payloadtypevalue == 201) {
    ret = _format_rtcp_rr(&pRtcpPkt[unRtcpOffset], unRtcpPktLen - unRtcpOffset, 
              &pszLog[unLogOffset], unLogBufLen - unLogOffset, unRcount);
  }
  else { 
    sprintf( pszLog + unLogOffset,  
        DIV_LINE "PT(%d) unknown. can't parse.\n",payloadtypevalue );
  }
  unLogOffset = strlen(pszLog); 

  if (ret == -1)
    sprintf(pszLog + unLogOffset, LAST_DIV_LINE);
  else { 
    sprintf(pszLog + unLogOffset, DIV_LINE);
  }
  unLogOffset += LINE_LENGTH; 

  ////////// RTCP Hex Dumping //////////
  for (unRtcpOffset = 0; unLogBufLen > unLogOffset + LINE_LENGTH*2; unLogOffset += LINE_LENGTH) {

    if (unRtcpPktLen >= unRtcpOffset+16) {
      sprintf(pszLog + unLogOffset, 
          "%02x %02x %02x %02x "
          "%02x %02x %02x %02x "
          "%02x %02x %02x %02x "
          "%02x %02x %02x %02x\n",
          pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], 
          pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], 
          pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], 
          pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++], pRtcpPkt[unRtcpOffset++] );
    }
    else {

      if (unRtcpOffset > 0 ) --unRtcpOffset;
      for (; unRtcpPktLen > unRtcpOffset; unLogOffset += 3) {
        sprintf(pszLog + unLogOffset,"%02x ", pRtcpPkt[unRtcpOffset++]); 
      }

      break;
    }
  }  

  ////////// END LINE //////////
  if (unRtcpPktLen > unRtcpOffset) {
    sprintf(pszLog + unLogOffset,"%s" LAST_DIV_LINE,
              pszLog[unLogOffset-1]=='\n' ? "" : "\n" );
  }
  else { 
    sprintf(pszLog + unLogOffset,"%s" DIV_LINE,
              pszLog[unLogOffset-1]=='\n' ? "" : "\n" );
  }

  return 0;
}

int _format_rtcp_rr(char *pRtcpPkt, unsigned int unRtcpPktLen,
    char *pszLog, unsigned int unLogBufLen, unsigned int unRcount)
{
  unsigned int i;
  unsigned int unRtcpOffset = 0;
  unsigned int unLogOffset = 0;

  if (pszLog == NULL || unLogBufLen < RTCP_BUF_MINIMUM) {
    return -1;
  }

  if ((unRcount*24) > unRtcpPktLen) {
    sprintf(pszLog + unLogOffset, 
        DIV_LINE "Report Block Start \nParse Error(too short)\n");
    return -2;
  }

  for (i = 0 ; i < unRcount; ++i) {
    RTCPRReport_T *pRtcpRr  = (RTCPRReport_T*) &pRtcpPkt[unRtcpOffset];
    pRtcpRr->ssrc     = ntohl(pRtcpRr->ssrc);
    pRtcpRr->last_seq = ntohl(pRtcpRr->last_seq);
    pRtcpRr->jitter   = ntohl(pRtcpRr->jitter);
    pRtcpRr->lsr      = ntohl(pRtcpRr->lsr);
    pRtcpRr->dlsr     = ntohl(pRtcpRr->dlsr);
    unRtcpOffset += 24;

    if (unLogBufLen > unLogOffset + LINE_LENGTH*10) {
      sprintf( pszLog + unLogOffset,
          DIV_LINE
          "Report Block %d Start\n"
          "SSRC_%d : %u\n"
          "Fraction Lost : %d\n"
          "Cumulative number of packets lost : 0:%d, 1:%d, 2:%d\n"
          "Extended highest seq number received : %u\n"
          "Interarrival jitter : %u\n"
          "last SR(LSR) : %u\n"
          "delay since last SR(DLSR): %u\n",
          i+1,
          i+1, pRtcpRr->ssrc,
          pRtcpRr->fract_lost,
          pRtcpRr->total_lost_0,
          pRtcpRr->total_lost_1,
          pRtcpRr->total_lost_2,
          pRtcpRr->last_seq,
          pRtcpRr->jitter,
          pRtcpRr->lsr,
          pRtcpRr->dlsr
      );
      unLogOffset = strlen(pszLog);
    }
  }

  return 0 ;
}


int _format_rtcp_sr(char *pRtcpPkt, unsigned int unRtcpPktLen,
      char *pszLog, unsigned int unLogBufLen, unsigned int unRcount)
{
  unsigned int unRtcpOffset = 0;
  unsigned int unLogOffset = 0;

  if (pszLog == NULL || unLogBufLen < RTCP_BUF_MINIMUM) {
    return -1;
  }

  if (unRtcpPktLen < 20) {
    sprintf(pszLog + unLogOffset, 
        DIV_LINE "Sender Report Start\nParse Error(too short)\n");
    return -2;
  }

  // Sender info start
  RTCPSReport_T *pRtcpSr  = (RTCPSReport_T*) pRtcpPkt;
  pRtcpSr->ntp_sec        = ntohl(pRtcpSr->ntp_sec);
  pRtcpSr->ntp_frac       = ntohl(pRtcpSr->ntp_frac);
  pRtcpSr->rtp_ts         = ntohl(pRtcpSr->rtp_ts);
  pRtcpSr->sender_pcount  = ntohl(pRtcpSr->sender_pcount);
  pRtcpSr->sender_bcount  = ntohl(pRtcpSr->sender_bcount);
  unRtcpOffset += 20;

  sprintf(pszLog + unLogOffset,
      DIV_LINE
      "Sender Info Start\n"
      "NTP Timestamp(MSW)    : H'%x(%u)\n"
      "NTP Timestamp(LSW)    : H'%x(%u)\n"
      "RTP Timestamp         : H'%x(%u)\n"
      "Sender's Packet Count :  %u\n"
      "Sender's Octet Count  :  %u\n",
      pRtcpSr->ntp_sec, pRtcpSr->ntp_sec,
      pRtcpSr->ntp_frac , pRtcpSr->ntp_frac,
      pRtcpSr->rtp_ts, pRtcpSr->rtp_ts,
      pRtcpSr->sender_pcount,
      pRtcpSr->sender_bcount
  ); 
  unLogOffset = strlen(pszLog); 

  return _format_rtcp_rr(&pRtcpPkt[unRtcpOffset], unRtcpPktLen - unRtcpOffset, 
            &pszLog[unLogOffset], unLogBufLen - unLogOffset, unRcount);
}


int main(char* argv[], int argc)
{

  char *pRtpPkt, *pRtcpPkt; 
  char *pszRtpLog, *pszRtcpLog; 
  unsigned int unRtpPktLen, unRtcpPktLen;
  unsigned int unRtpLogLen, unRtcpLogLen;

  printf("Test Start \n");

  unRtpPktLen  = 1500;
  unRtcpPktLen = 1024;

  unRtpLogLen  = (unRtpPktLen  * 3) + (RTP_BUF_MINIMUM  * 2);
  unRtcpLogLen = (unRtcpPktLen * 3) + (RTCP_BUF_MINIMUM * 4);

  pRtpPkt  = malloc(unRtpPktLen);
  pRtcpPkt = malloc(unRtcpPktLen);
  memset(pRtpPkt, 0x01, unRtpPktLen);
  memset(pRtcpPkt, 0x01, unRtcpPktLen);

  pszRtpLog = malloc(unRtpLogLen);
  pszRtcpLog= malloc(unRtcpLogLen);
  memset(pszRtpLog, 0x00, unRtpLogLen);
  memset(pszRtcpLog, 0x00, unRtcpLogLen);

  make_rtp_log  (pRtpPkt, unRtpPktLen, pszRtpLog, unRtpLogLen);
  make_rtcp_log (pRtcpPkt, unRtcpPktLen, pszRtcpLog, unRtcpLogLen);

  printf("#####################TEST RTP###################\n");  
  printf("%s\n", pszRtpLog);
  printf("################################################\n");  
  printf("#####################TEST RTCP##################\n");  
  printf("%s\n", pszRtcpLog);
  printf("################################################\n");  

  free(pRtpPkt);
  free(pRtcpPkt);
  free(pszRtpLog);
  free(pszRtcpLog);

  return 0;
}

