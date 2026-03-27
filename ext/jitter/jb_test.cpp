#include <stdio.h>
#include <stdlib.h>
#include "CJBuffer.h"

using namespace PICO;

int main(int argc,char * argv[])
{
   printf("jitter buffer test program\n");

   CJBuffer jbuff;
   JBZoneTrace_T JB_ZoneTrace;
   JBParam_T   JB_Param     ; // Jitter Buffer Parameter
   unsigned long JB_StartTimeMSec;
   unsigned long JB_FetchCount;
   unsigned char JB_Used      ; // 0: NO-JB, 1:FJB, 2:AJB
   unsigned char JB_BypassDTMF; // DTMF bypass mode(now not used)  
   unsigned char JB_BypassSID ; // SID  bypass mode(now not used)
   unsigned int  JB_TalkSepMSec;
   unsigned int nCSID;


/*******
commercial site configure 
[jbuffer]
method          = 2
bypass_dtmf     = 1
bypass_sid      = 0
seq_dropout_max = 150
seq_misorder_max= 2
buffer_max      = 10
prefetch_min    = 4
prefetch_max    = 5
allow_undermin  = 0
use_tsasindex   = 1

drops_on_full   = 0
use_adjust      = 1
talksep_msec    = 300
# 1:delete only,2:control,3:alive,netfail
print_end_log   = 0

monitor_ip      = 0.0.0.0
monitor_port    = 0

#
# 0 : all off
# 1 : ilevel, info only
# 2 : alevel, api level
# 3 : dlevel, debug only
tlog_level = 0
****/
   // Jitter Buffer Parameter
   JB_StartTimeMSec = 0;
   JB_FetchCount = 0;
   JB_Used  = 1       ;
   JB_BypassDTMF = 1  ;
   JB_BypassSID  = 0  ;
   JB_TalkSepMSec= 300;
   JB_Param.Reset()   ;


   PiResult_T ret;
   //Add code when rtpcomm start
   JB_Param.UseAdaptive = 0;
   JB_Param.AllowUnderMin = 0;
   JB_Param.DropoutMax = 150;
   JB_Param.MisorderMax = 2;
   JB_Param.MinPrefetch = 3;
   JB_Param.MaxPrefetch = 4;
   JB_Param.InitPrefetch = 1;
   JB_Param.BufferMax = 10;
   JB_Param.UseTsAsIndex = 1;
   JB_Param.DropsOnResShort = 0;
   JB_Param.UseAdjust = 1;
   JB_Param.CSID = 100;

   if(JB_Used)
   {
      ret = jbuff.Init(&JB_Param);
      if(ret != PiSuccess)
      {
         printf("JB Init failed. res+%d, go no jb mode",ret);
         JB_Used = 0;
         JB_StartTimeMSec = 0;
         JB_FetchCount = 0xFFFFFFFF; // guard infinite loop
      } 
      else
      {
         JB_StartTimeMSec = GetNowTimestampMS();
         JB_FetchCount = 0;
         JB_ZoneTrace.Init(nCSID,JB_TalkSepMSec);
         JB_ZoneTrace.Reset();
      }
         
   }

   //1. Recive 
   //when packet receiveed, putframe at buffer code
   JBFrameDesc_T desc;
   desc.rxtime_msec = GetNowTimestampMS();
   desc.frmtype = E_JBFrameType_Normal;
   desc.pktlen = 45;//pktlen;
   desc.seqnumber = 100;//sequenct number
   desc.timestamp = 100;//timestamp;
   desc.pt = 98;//paylaod type
   desc.marker = 1;//marker
   desc.isdtmf = 1;//set dtmf chk indicator 
   desc.isspeech = 1;// when amr-nb frame type below than 7 , it is speech
   desc.index = -1;

   unsigned char discarded=0;
   char frame[]={0x3c,0x1e,0x3c,0x38, 0x40,0x4b,0x88,0x66,0x68,0x4a,0xc4,0xcb,0x24,0x42,0x44,0x72,0x40,0x00,0x41,0x63,0x50,0x30,0x34,0xa0,0x00,0x00,0x66,0x80,0x45,0x42,0x0b,0x20};

   ret = jbuff.PutFrame(JB_ZoneTrace.IsSpeechZone, &desc, frame, &discarded);
   
   if( (ret!=PiSuccess)&&
        (ret!=PiRErrAlready) &&
        (ret!=PiRErrTooLate) ){
        printf("JB PutFrame Failed. res=%d",ret);
        return PiRErrJBSysfail ;
    }

/* out reasoncode setting part .. refrence code

   if(result==PiRErrAlready){
        UPDSTATS(total.drop_dup);
        *oresult = E_PktChkRes_BadOrder ;
    }else if(result==PiRErrTooLate){
        UPDSTATS(total.drop_outorder);
        *oresult = E_PktChkRes_BadOrder ;
    }else{ // success
        if( *opt2ind == 0 ){
            UPDSTATS(total.rx_pt1);
            UPDSTATS_BYTES(total.rx_pt1_bytes,pktlen);
        }else{
            UPDSTATS(total.rx_pt2);
            UPDSTATS_BYTES(total.rx_pt2_bytes,pktlen);
        }
        *oresult = E_PktChkRes_JB_Processed ;
    }
*/

   printf("PutFrame success\n");


   //2. Get Frame
   unsigned long expcount;
   char frame_get[1500];
   memset(frame_get,0x00,sizeof(frame_get));

   unsigned long nowtime = GetNowTimestampMS();
   expcount = (nowtime - JB_StartTimeMSec)/20L; 
   JBFrameDesc_T desc_get;
   JB_FetchCount++;
   ret = jbuff.GetFrame(&desc_get,frame_get,1460);
   if(ret != PiSuccess)
   {
      printf("JB GetFrame failed. res=%d\n", ret);
      return PiRErrJBSysfail;
   }

   if(desc_get.frmtype==E_JBFrameType_Missing){
        JB_ZoneTrace.Update(0);
        //*oresult = E_PktChkRes_JB_NotData ;
    }else if(desc_get.frmtype==E_JBFrameType_ZeroPrefetch){
        JB_ZoneTrace.Update(0);
        //*oresult = E_PktChkRes_JB_NotData ;
    }else if(desc_get.frmtype==E_JBFrameType_ZeroEmpty){
        JB_ZoneTrace.Update(0);
        //*oresult = E_PktChkRes_JB_NotData ;
    }else if(desc_get.frmtype==E_JBFrameType_Normal){
        JB_ZoneTrace.Update(desc_get.isspeech);
        //*oresult = E_PktChkRes_RTP        ;
    }else {
        printf("SOMETHING WRONG JB FT(%d) unknown\n",desc_get.frmtype);
        //*oresult = E_PktChkRes_BadPkt ;
        return PiRErrJBSysfail ;
    }

   printf("GetFrame Success\n");
}

