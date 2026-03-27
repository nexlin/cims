#include "CJBuffer.h"
#include <stdarg.h>

namespace PICO{

#define RTP_SEQ_MOD     (1 << 16)

#define LD_MASK_CFG        0x00000001
#define LD_MASK_JB_STATUS  0x00000002
#define LD_MASK_FL_STATUS  0x00000004 
#define LD_MASK_FL_FRAMES  0x00000008
#define LD_MASK_JB_STATS   0x00000010
#define LD_MASK_FL_STATS   0x00000020
#define LD_MASK_STATUS    (LD_MASK_JB_STATUS|LD_MASK_FL_STATUS)
#define LD_MASK_STATS     (LD_MASK_JB_STATS |LD_MASK_FL_STATS )
#define LD_MASK_ALL       (0xFFFFFFFF)

//static pisb32 _g_JBTLogLevel = 3 ;
//static pisb32 _g_JBTLogLevel = 1 ;
static pisb32 _g_JBTLogLevel = 0 ;

void jb_change_tlog_level(pisb32 level){

    if(_g_JBTLogLevel!=level){
        fprintf(stderr,"JB Trace Log level changed %d -> %d",
            _g_JBTLogLevel,level);
        _g_JBTLogLevel = level ;
    }

}

unsigned long GetNowTimestampMS(void){
    unsigned long stamp ;
    struct timeval tv ;

    gettimeofday(&tv,NULL);

    stamp = tv.tv_sec*1000000 + tv.tv_usec ;

    stamp = stamp/1000 ;

    return stamp ;
}

unsigned long GetNowTimestampUS(void){
    unsigned long stamp ;
    struct timeval tv ;

    gettimeofday(&tv,NULL);

    stamp = tv.tv_sec*1000000 + tv.tv_usec ;

    stamp = stamp ;

    return stamp  ;
}

PiResult_T CJBFrameList::Init(JBParam_T*param){

    //LogPrint(JBTL_DBG,"Init CALLED");
    CSID           = param->CSID          ;

    m_MaxCount     = param->BufferMax     ;
    m_MaxFrameSize = JB_FRAME_CONTENT_MAX ;
    m_MaxDropout   = param->DropoutMax    ;
    m_MaxMisorder  = param->MisorderMax   ;

    return Reset(1);
}

PiResult_T CJBFrameList::Reset(pibyte isinitial){

    //LogPrint(JBTL_DBG,"Reset CALLED initial=%d",isinitial);

    m_Origin         = -9999 ;
    m_HeadIdx        =0 ;
    m_Size           =0 ;
    m_DiscardedNum   =0 ;

    for(unsigned i = 0 ; i < m_MaxCount ; i++){
        if(!isinitial){
            if(m_Array[i].type==E_JBFrameType_Normal){
                if(m_Array[i].isspeech){
                    m_Stats.ResetDropSpeech++;
                }else{
                    m_Stats.ResetDropSID++;
                }
            }
        }
        m_Array[i].Init();
    }

    if(isinitial){
        m_Stats.Reset();
    }

    return PiSuccess ;  
}

piub32 CJBFrameList::RemoveHead(piub32 count){

    LogPrint(JBTL_DBG,"RemoveHead CALLED count=%u,now orig=%d,hidx=%d,size=%d",
        count, m_Origin, m_HeadIdx, m_Size);

    if(count > m_Size){
        count = m_Size ;
    }

    if(count > JB_FRAME_LIST_MAX){
        //LOGGER(PL_CRITICAL,"JB RemoveHead Assert(count(%u)>LIST_MAX) Failed",
            //count);
    }

    if(count){
        unsigned loopcnt,i;

        for(i = m_HeadIdx,loopcnt = count  ; loopcnt > 0 ;loopcnt--){

            if(m_Array[i].type==E_JBFrameType_Discarded){
                m_Stats.RemoveDiscarded++;
                if(m_DiscardedNum>0){
                    m_DiscardedNum--;
                }else{
                    // something wrong.
                }
            }else if(m_Array[i].type==E_JBFrameType_Normal){
                if(m_Array[i].isspeech){
                    m_Stats.RemoveSpeech++;
                }else{
                    m_Stats.RemoveSID++;
                }
            }else if(m_Array[i].type==E_JBFrameType_Missing){
                m_Stats.RemoveMissing++ ;
            }

            m_Array[i].Init();

            i = (i+1)%m_MaxCount ;
        }

        m_Origin += count ;
        m_HeadIdx = (m_HeadIdx+count)%m_MaxCount ; 
        m_Size   -= count ;
    }

    if(m_Size > JB_FRAME_LIST_MAX){
       //LOGGER(PL_CRITICAL,"JB RemoveHead Assert(m_Size(%u)>LIST_MAX) Failed",
       //m_Size); 
    }

    LogPrint(JBTL_DBG,"RemoveHead Ended ret count=%u,now orig=%d,hidx=%d,size=%d",
        count, m_Origin, m_HeadIdx, m_Size);

    return count ;
}

piub32 CJBFrameList::Adjust(piub32 minprefetch,pisb32 curputidx){

    piub32 count,removed ;
    unsigned loopcnt,i;

    count = m_Size  - minprefetch ;

    LogPrint(JBTL_API,
    "Adjust CALLED orgin=%d,size=%d,minp=%d,curputidx=%d,rmcount=%d",
    m_Origin,m_Size,minprefetch,curputidx,count);


    if(count > m_Size){
        LogPrint(JBTL_API,"Adjust count changed (%d)>m_Size(%d)",
            count,m_Size);
        count = m_Size ;
    }


    if((m_Origin+(int)count)>curputidx){
        // something wrong.  
        // jitter buffer state is abnormal
        LogPrint(JBTL_INF,"SOMETHING WRONG :Adjust Origin(%d) + count(%d) > curputidx(%d)",
            m_Origin,(int)count,curputidx);
    }

    if(count > JB_FRAME_LIST_MAX){
        //LOGGER(PL_CRITICAL,"JB RemoveHead Assert(count(%u)>LIST_MAX) Failed",
            //count);
    }

    if(count){

        removed=0;

        for(i = m_HeadIdx,loopcnt = count  ; loopcnt > 0 ;loopcnt--){

            if(m_Array[i].type==E_JBFrameType_Discarded){
                m_Stats.RemoveDiscarded++;
                if(m_DiscardedNum>0){
                    m_DiscardedNum--;
                }else{
                    // something wrong.
                }
            }else if(m_Array[i].type==E_JBFrameType_Normal){
                if(m_Array[i].isspeech){
                    // normal speech occurred. 
                    break ;
                }else{
                    m_Stats.RemoveSID++;
                }
            }else if(m_Array[i].type==E_JBFrameType_Missing){
                m_Stats.RemoveMissing++ ;
            }

            removed++;

            m_Array[i].Init();
            i = (i+1)%m_MaxCount ;
        }

        m_Origin += removed ;
        m_HeadIdx = (m_HeadIdx+removed)%m_MaxCount ; 
        m_Size   -= removed ;
    }

    if(m_Size > JB_FRAME_LIST_MAX){
       //LOGGER(PL_CRITICAL,"JB Adjust Assert(m_Size(%u)>LIST_MAX) Failed",
       //m_Size); 
    }

    LogPrint(JBTL_API,
        "Adjust remove %d frames,origin=%d,size=%d",
        removed,m_Origin,m_Size);

    return removed ;
}

PiResult_T CJBFrameList::Discard(int index){

    unsigned pos ;

    if((index >= m_Origin) && (index < m_Origin + (int)m_Size)){
        return PiRError ;
    }

    pos = (m_HeadIdx + (index - m_Origin))%m_MaxCount ;

    m_Array[pos].type = E_JBFrameType_Discarded ;
    m_DiscardedNum++;

    return PiSuccess ;
}

PiResult_T CJBFrameList::Peek( 
    piub32 offset,
    JBFrameDesc_T*odesc,
    pibyte*oFrameBuffer,
    piub32 FrameBufferLen){

    piub32 pos,idx ;

    if(offset >= EffSize()){
/*
#ifdef _JBUF_DEBUG_
        LOGGER(PL_JBUF,"JB FL Peek off=%d, effsize=%d return Error",
            offset,EffSize());
#endif
*/
        return PiRError ;
    }

    pos = m_HeadIdx ;
    idx = offset ;

    while(1){
        if(m_Array[pos].type != E_JBFrameType_Discarded){
            if(idx==0){
                break ;
            }else{
                --idx;
            }
        }
        pos = (pos + 1)%m_MaxCount ;
    }

    // description
    odesc->rxtime_msec= m_Array[pos].rxtime_msec ;
    odesc->frmtype    = m_Array[pos].type ;

    // normal frame 
    if(m_Array[pos].type==E_JBFrameType_Normal){
        if( (oFrameBuffer!=NULL)&&(FrameBufferLen>=m_Array[pos].contentlen)&&
            (m_Array[pos].contentlen>0) ){
            // copy frame data
            memcpy(oFrameBuffer,m_Array[pos].content,m_Array[pos].contentlen);
        }else{
            if(FrameBufferLen< m_Array[pos].contentlen){
                //LOGGER(PL_ERROR,
                //"JB FL Peek(%p,%p,%d) fl.pos=%d cotlen=%d too large",
                //odesc,oFrameBuffer,FrameBufferLen,pos,m_Array[pos].contentlen);
            }
        }
    }

    odesc->pktlen     = m_Array[pos].contentlen;

    odesc->seqnumber  = m_Array[pos].seqnum ;
    odesc->timestamp  = m_Array[pos].timestamp;
    odesc->pt         = m_Array[pos].pt ;
    odesc->isdtmf     = m_Array[pos].isdtmf ;
    odesc->index      = m_Origin ;

    return PiSuccess ;
}

// success : success
// invalidp: sysfail
// toobig  : sysfail
// Again   : no frame data, buffer is empty
PiResult_T CJBFrameList::Get( 
    JBFrameDesc_T*odesc,
    pibyte*oFrameBuffer,
    piub32 FrameBufferLen){

    PiResult_T result = PiSuccess ;

    //LogPrint(JBTL_DBG,
    //"Get CALLED orgin=%d,size=%d",m_Origin,m_Size);

    if(m_Size){
        pibyte prev_discarded = 0 ;
  
        // remove discarded frame at first missing or normal.
        while(m_Array[m_HeadIdx].type==E_JBFrameType_Discarded){
            RemoveHead(1);
            prev_discarded =1 ;
        }
       
        if(m_Size){
            // description set
            odesc->rxtime_msec= m_Array[m_HeadIdx].rxtime_msec ;
            odesc->frmtype    = m_Array[m_HeadIdx].type ;
            odesc->pktlen     = 0 ;

            // pktlen and frame data copy
            if(m_Array[m_HeadIdx].type==E_JBFrameType_Normal){
                if(m_Array[m_HeadIdx].isspeech){
                    m_Stats.GetSpeech++;
                }else{
                    m_Stats.GetSID++;
                }
                
                if( (oFrameBuffer!=NULL)&&
                    (FrameBufferLen>=m_Array[m_HeadIdx].contentlen)&&
                    (m_Array[m_HeadIdx].contentlen>0) ){

                    // copy frame data
                    memcpy(oFrameBuffer,m_Array[m_HeadIdx].content,
                        m_Array[m_HeadIdx].contentlen);

                    odesc->pktlen = m_Array[m_HeadIdx].contentlen ;

                    result = PiSuccess ;
                }else{
                    result = PiRErrInvalidParam ;
                    if(FrameBufferLen< m_Array[m_HeadIdx].contentlen){
#ifdef _DEBUG_JBUF_                     
                        //LOGGER(PL_CRITICAL,
                        //"JB FL Get(%p,%p,%d) fl.pos=%d cotlen=%d too large",
                        //odesc,oFrameBuffer,FrameBufferLen,
                        //m_HeadIdx,m_Array[m_HeadIdx].contentlen);
#endif                        
                        result = PiRErrTooBig ; 
                    }
                }
            }else{
                m_Stats.GetMissing++;
                // Missing Frame
                // redundant checking
                if(odesc->frmtype!=E_JBFrameType_Missing){
                    //LOGGER(PL_CRITICAL,"SOMETHING WRONG at %d:%s",
                        //__FILE__,__LINE__);
                    //LOGGER(PL_CRITICAL,"JB FL frmtype %d detected",
                        //odesc->frmtype);
                }

                odesc->frmtype = E_JBFrameType_Missing ;
                result = PiSuccess ;
            }

            odesc->seqnumber  = m_Array[m_HeadIdx].seqnum ;
            odesc->timestamp  = m_Array[m_HeadIdx].timestamp;
            odesc->pt         = m_Array[m_HeadIdx].pt ;
            odesc->isdtmf     = m_Array[m_HeadIdx].isdtmf ;
            odesc->isspeech   = m_Array[m_HeadIdx].isspeech;
            odesc->index      = m_Origin ;
            
            // get first frame's information reset
            m_Array[m_HeadIdx].Init();

            // update list status.
            m_Origin++;
            m_HeadIdx = (m_HeadIdx+1)%m_MaxCount ;
            m_Size--; 

            LogPrint(JBTL_DBG,"Get ended ret=%d ft=%d,seq=%d,ts=%d,idx=%d,len=%d",
                result,
                odesc->frmtype,
                odesc->seqnumber,
                odesc->timestamp,
                odesc->index,
                odesc->pktlen);

            return result ;
        }
    }

    LogPrint(JBTL_DBG,"FL Empty State ret=ErrAgain");

    // No Frame Available.
    m_Stats.GetZeroEmpty++;

    return PiRErrAgain ;
}

// resshort(overrun),already(duplicated),toolate(drop packet due to mis order)
// put sys fail error : invalidparam,TooBig 
PiResult_T CJBFrameList::PutAt(
    JBFrameDesc_T*desc, pibyte*frame,pibyte*oreseted){

    pisb32 distance ;
    piub32 pos      ;

    if(desc==NULL){
        printf("JB FL PutAt desc is NULL\n");
        return PiRErrInvalidParam ;
    }

    if( (desc->frmtype!= E_JBFrameType_Normal) &&
        (desc->frmtype!= E_JBFrameType_Missing) ){
        printf("JB FL PutAt frametype(%d) unexpected\n",
            desc->frmtype);
        return PiRErrInvalidParam ; 
    }

    if((frame==NULL) && (desc->pktlen!=0)){
        printf("JB FL frame is null pktlen(%d) is not zero\n",
            desc->pktlen);
        return PiRErrInvalidParam ;
    }

    if((frame!=NULL) && (desc->pktlen>JB_FRAME_CONTENT_MAX)){
        printf("JB FL PutAt pktlen(%d) too big\n",
            desc->pktlen);
        return PiRErrTooBig ;
    }

    // sequence restarted checking 
    *oreseted = 0 ;

    LogPrint(JBTL_DBG,
        "PutAt CALLED seq=%d,ts=%u,index=%d(orign=%d,hidx=%d,size=%d)",
        desc->seqnumber,desc->timestamp,desc->index,m_Origin,m_HeadIdx,m_Size);

    if(desc->index < m_Origin){

        if(m_Origin - desc->index < m_MaxMisorder ){
            // too late packet drop packet.
            LogPrint(JBTL_DBG,"PutAt seq=%d,ts=%u,index=%d(orign=%d) too late",
                desc->seqnumber,desc->timestamp,desc->index,m_Origin);
                
            if(desc->isspeech){
                m_Stats.PutTooLateSpeech++;
            }else{
                m_Stats.PutTooLateSID++;
            }

            return PiRErrTooLate ;
        }else{
#if 0
            // sequence restarted, uphold buffering size
            // m_Origin = index - m_Size ;
#else

            printf("JB FL reset(backward jump)\n");

            LogPrint(JBTL_INF,
            "Reset(bw jump):desc(index=%d,seq=%d,ts=%u) m_Origin=%d,MaxMiso=%d",
                desc->index,
                desc->seqnumber,
                desc->timestamp,
                m_Origin,m_MaxMisorder); 

            Reset(0);
            m_Origin = desc->index ;
            *oreseted = 1 ;
            m_Stats.ResetBackward++;
#endif
        }
    }

    if(m_Size==0){
        m_Origin = desc->index ;
        m_Stats.OriginResetZeroSize++;
    }

    distance = desc->index - m_Origin ;

    if(distance >= (int)m_MaxCount){
        if(distance>m_MaxDropout){
            // too long jump
            // sequence restarted. 
            LogPrint(JBTL_INF,
            "Reset(fw jump):desc(index=%d,seq=%d,ts=%u) dist=%d,Origin=%d,Size=%d,MaxDrp=%d",
                desc->index,
                desc->seqnumber,
                desc->timestamp,
                distance,
                m_Origin,
                m_Size,
                m_MaxDropout); 

            m_Stats.ResetForward++;
            Reset(0);
            m_Origin = desc->index ;
            distance = 0 ;
            *oreseted= 1 ;
        }else{
            // too many, reject the frame.
            // upper layer discard oldest frames, and retry put
            LogPrint(JBTL_INF,
            "ResShort:desc(index=%d,seq=%d,ts=%u) distance=%d,Origin=%d,Size=%d,MaxDrp=%d",
                desc->index,
                desc->seqnumber,
                desc->timestamp,
                distance,
                m_Origin,
                m_Size,
                m_MaxDropout); 
#if 0
            Print(m_MaxCount);
#endif
            if(desc->isspeech){
                m_Stats.PutResShortSpeech++;
            }else{
                m_Stats.PutResShortSID++;
            }
            return PiRErrResShortage ;
        }
    }

    pos = (m_HeadIdx + distance)%m_MaxCount ;

    if(m_Array[pos].type != E_JBFrameType_Missing){

        if(desc->isspeech){
            m_Stats.PutDupSpeech++;
        }else{
            m_Stats.PutDupSID++;
        }

        // duplicated frame 

        LogPrint(JBTL_INF,
            "Dup:desc(index=%d,seq=%d,ts=%u) distance=%d,Origin=%d,Size=%d,MaxDrp=%d",
            desc->index,
            desc->seqnumber,
            desc->timestamp,
            distance,
            m_Origin,
            m_Size,
            m_MaxDropout); 

        return PiRErrAlready ;
    }

    m_Array[pos].type        = desc->frmtype ;
    m_Array[pos].rxtime_msec = desc->rxtime_msec ; 

    if(desc->frmtype==E_JBFrameType_Normal){
        m_Array[pos].contentlen  = desc->pktlen ;
        if(desc->pktlen>0){
            memcpy(m_Array[pos].content,frame,desc->pktlen);
        }
        if(desc->isspeech){
            m_Stats.PutSpeech++;
        }else{
            m_Stats.PutSID++;
        }
    }else{
        // this code is not used in our app.
        m_Array[pos].contentlen  =  0;
    }

    m_Array[pos].seqnum    = desc->seqnumber;
    m_Array[pos].timestamp = desc->timestamp;
    m_Array[pos].pt        = desc->pt       ;
    m_Array[pos].marker    = desc->marker   ;
    m_Array[pos].isdtmf    = desc->isdtmf   ;
    m_Array[pos].isspeech  = desc->isspeech ;

    if(m_Origin + (int)m_Size <= desc->index){
        m_Size = distance + 1 ;
    }

    LogPrint(JBTL_DBG,
        "PutAt ended (orign=%d,hidx=%d,size=%d)",m_Origin,m_HeadIdx,m_Size);

    return PiSuccess ;
}

void CJBFrameList::LogPrint(piub32 level,char*fmt,...){
    pichar line[1024];
    va_list arg_list    ;

    if((pisb32)level > _g_JBTLogLevel){
        return ;
    }

    va_start(arg_list,fmt);
    vsnprintf(line,1023,fmt,arg_list);
    va_end(arg_list);

    fprintf(stderr,"CSID(%5d):FLLOG:%s\n",CSID,line);
}

PiResult_T CJBuffer::Init(JBParam_T* param){

    PiResult_T result ;

    // Parameter validate checking
    if(param->BufferMax > JB_FRAME_LIST_MAX){
        //LOGGER(PL_ERROR,"JB Init: BufferMax(%d) > %d",
            //param->BufferMax,JB_FRAME_LIST_MAX);
        return PiRErrInvalidParam ;
    }

    if(param->BufferMax<=0){
        //LOGGER(PL_ERROR,"JB Init: BufferMax(%d) <= 0",param->BufferMax);
        return PiRErrInvalidParam ;
    }

    if(param->PTime <=0){
        //LOGGER(PL_ERROR,"JB Init: PTime(%d) <= 0",param->PTime);
        return PiRErrInvalidParam ;
    }

    if( (param->DropoutMax <JB_MAXDROPOUT_MIN)||
        (param->DropoutMax >JB_MAXDROPOUT_MAX)) {
        //LOGGER(PL_ERROR,"JB Init: DropoutMax(%d) < %d or > %d",
            //param->DropoutMax,JB_MAXDROPOUT_MIN,JB_MAXDROPOUT_MAX);
        return PiRErrInvalidParam ;
    }

    if( (param->MisorderMax <JB_MAXMISORDER_MIN)||
        (param->MisorderMax >JB_MAXMISORDER_MAX)) {
        //LOGGER(PL_ERROR,"JB Init: MisorderMax(%d) < %d or > %d",
            //param->MisorderMax,JB_MAXMISORDER_MIN,JB_MAXMISORDER_MIN);
        return PiRErrInvalidParam ;
    }

    if(param->UseAdaptive==0){
        if(param->InitPrefetch > (param->BufferMax*3/4)){
            //LOGGER(PL_ERROR,"JB Init: InitPrefetch(%d) > (%d*3/4)",
                //param->InitPrefetch,param->BufferMax);
            return PiRErrInvalidParam ;
        }
    }else{
        if(param->MaxPrefetch > param->BufferMax){
            //LOGGER(PL_ERROR,"JB Init: MaxPrefetch(%d) > BufferMax(%d)",
                //param->MaxPrefetch,param->BufferMax);
            return PiRErrInvalidParam ;
        }
        if(param->MinPrefetch >=param->MaxPrefetch){
            //LOGGER(PL_ERROR,"JB Init: MinPrefetch(%d) >= MaxPrefetch(%d)",
                //param->MinPrefetch,param->MaxPrefetch);
            return PiRErrInvalidParam ;
        }
        if(param->InitPrefetch>=param->BufferMax){
            //LOGGER(PL_ERROR,"JB Init: InitPrefetch(%d) >= BufferMax(%d)",
                //param->InitPrefetch,param->BufferMax);
            return PiRErrInvalidParam ;
        }
    }

    // FrameList Initialize.
    result = m_FrameList.Init(param);
    if(result!=PiSuccess){
        //LOGGER(PL_ERROR,"JB FL init(%d,%d) failed. res=%d",
            //param->BufferMax,JB_FRAME_CONTENT_MAX,result);
        return result ;
    }

    // JBuffer Configuration Value Set
    m_Config.MaxDropout  = param->DropoutMax ;
    m_Config.MaxMisorder = param->MisorderMax ;
    m_Config.FramePTime  = param->PTime ;

    if(param->UseAdaptive==0){
        m_Config.InitPrefetch=m_Config.MinPrefetch
            =m_Config.MaxPrefetch =param->InitPrefetch ;
        m_Config.MaxBurst = 0 ;
        m_Config.AllowUnderMin  = 0 ;
        m_Config.DropsOnResShort= param->DropsOnResShort ;
        m_Config.UseTsAsIndex   = param->UseTsAsIndex?1:0  ; 
        m_Config.UseAdjust      = param->UseAdjust ;
    }else{
        m_Config.InitPrefetch   = param->InitPrefetch ;
        m_Config.MinPrefetch    = param->MinPrefetch  ;
        m_Config.MaxPrefetch    = param->MaxPrefetch  ;

        if(param->BurstMax == 0 ){
            m_Config.MaxBurst   = PT_MAX(param->BurstMax,param->BufferMax*3/4);
        }else{
            m_Config.MaxBurst = param->BurstMax ;
        }

        m_Config.AllowUnderMin  = param->AllowUnderMin?1:0 ;
        m_Config.DropsOnResShort= param->DropsOnResShort ;
        m_Config.UseTsAsIndex   = param->UseTsAsIndex?1:0  ; 
        m_Config.UseAdjust      = param->UseAdjust ;
    }

    m_Config.CSID = param->CSID ;

    m_Prefetch = param->InitPrefetch ;

    // State Reset(m_Prefetch is not reseted)
    Reset(1);

#if 0
    if(_g_JBTLogLevel==JBTL_DBG){
        LogPrint(JBTL_DBG,LD_MASK_ALL,"Inited");
    }else if(_g_JBTLogLevel==JBTL_API){
        LogPrint(JBTL_API,LD_MASK_CFG|LD_MASK_STATUS,"Inited");
    }else {
        LogPrint(JBTL_INF,LD_MASK_STATUS,"Inited");
    }
#endif

    return PiSuccess ;
}

void CJBuffer::Reset(pibyte isinitial){

    m_PutFrameCount      = 0 ;
    m_PutFrameRtpSeqMax  =-1 ;

    m_LastPutFrameRxTime = 0 ;
    m_LastPutFrameRtpTs  = 0 ;
    m_LastPutFrameRtpIsSpeech=0;

    m_State        = E_CJBState_Initializing ;
    m_InitCycleCnt = 0 ;
    m_Prefetching  = (m_Prefetch != 0);
    m_Level        = 0 ;
    m_LastOper     = E_CJBOper_Init ;
    m_MaxHistLevel = 0 ;
    m_StableHist   = 0 ;
    m_EffLevel     = 0 ;

    m_CurIndex     = 0 ;

    m_FrameList.Reset(isinitial);

    if(isinitial){
        m_Stats.Reset();
    }
}

void CJBuffer::CalculateJitter(void){
    int diff,cur_size ;

    //LogPrint(JBTL_API,LD_MASK_JB_STATUS,"CalcJitter CALLED");

    cur_size = m_FrameList.EffSize();

    // level history save
    m_MaxHistLevel = PT_MAX(m_MaxHistLevel,m_Level);

    if(m_Level < m_EffLevel){
        // burst level decrease.

        m_StableHist++;

        if(m_StableHist > 20){
            diff = (m_EffLevel - m_MaxHistLevel)/3 ;
            if(diff < 1){
                diff = 1 ;
            }

            m_EffLevel -= diff ;

            if(m_Config.InitPrefetch){
                m_Prefetch = m_EffLevel ;
                if(m_Prefetch < m_Config.MinPrefetch){
                    m_Prefetch = m_Config.MinPrefetch ;
                }
            }

            m_MaxHistLevel = 0 ;
            m_StableHist   = 0 ;
        }
    }else if(m_Level > m_EffLevel){
        // increase
        m_EffLevel = PT_MIN(m_MaxHistLevel,(int)(m_FrameList.MaxCount()*4/5));

        if(m_Config.InitPrefetch){
            m_Prefetch = m_EffLevel ;

            // added by MIND
            if(m_Config.AllowUnderMin==0){
                if(m_Prefetch < m_Config.MinPrefetch){
                    m_Prefetch = m_Config.MinPrefetch ;
                }
            }

            if(m_Prefetch > m_Config.MaxPrefetch){
                m_Prefetch = m_Config.MaxPrefetch ;
            }
        }
        m_StableHist = 0 ;
    }else{
        // level unchanged.
        m_StableHist = 0 ;
    }

    //LogPrint(JBTL_API,LD_MASK_JB_STATUS,"CalcJitter Ended");
}

void CJBuffer::Update(E_CJBOper_T oper){

    if(m_LastOper != oper){

        //LogPrint(JBTL_API,LD_MASK_JB_STATUS,"Update CALLED");

        m_LastOper = oper ;

        if(m_State==E_CJBState_Initializing){
            /* Switch status 'initializing' -> 'processing' after some OP
             * switch cycles and current OP is GET (burst level is calculated
             * based on PUT burst), so burst calculation is guaranted to be
             * performed right after the status switching.
             */
            if(++m_InitCycleCnt >= JB_INIT_CYCLE && oper==E_CJBOper_Get){
                m_State = E_CJBState_Progressing ;
                //LOGGER(PL_JBUF,"JB STATE CHANGED to PROGRESSING");
                /* To make sure the burst calculation will be done right after
                 * this, adjust burst level if it exceeds max burst level.
                 */
                m_Level = PT_MIN(m_Level,m_Config.MaxBurst);
            }else{
                m_Level = 0 ;
                //LogPrint(JBTL_API,LD_MASK_JB_STATUS,"Update ended(in init)");
                return ;
            }
        }

        /* Perform jitter calculation based on PUT burst-level only, since
         * GET burst-level may not be accurate, e.g: when VAD is active.
         * Note that when burst-level is too big, i.e: exceeds jb_max_burst,
         * the GET op may be idle, in this case, we better skip the jitter
         * calculation.
         */
         if(oper==E_CJBOper_Get && m_Level <= m_Config.MaxBurst){
             CalculateJitter();
         }

         m_Level = 0 ;
    }
    //LogPrint(JBTL_API,LD_MASK_JB_STATUS,"Update ended");
}

PiResult_T CJBuffer::PutFrame(
    pibyte isspeechzone,
    JBFrameDesc_T*desc,pibyte*frame,pibyte*odiscarded){

    piub16 delta ;
    pibyte reseted;

    PiResult_T result ;
    pisb32 new_size,cur_size;
    piub16  seqnumber=0 ;
    pibyte  misordered=0,duplicated=0;

    *odiscarded = 1 ;

    m_Stats.SeqReceived++ ;


    if(desc==NULL){
        printf("JB PutFrame desc null.\n");
        return PiRErrInvalidParam ;
    }

    if( (desc->frmtype!= E_JBFrameType_Normal) &&
        (desc->frmtype!= E_JBFrameType_Missing) ){
       printf("JB PutFrame frtmtype(%d) invalid\n",
            desc->frmtype);
        return PiRErrInvalidParam ; 
    }

    if((frame==NULL) && (desc->pktlen!=0)){
        printf("JB PutFrame frame=%p, pktlen=%d\n",
            frame,desc->pktlen);
        return PiRErrInvalidParam ;
    }

    if((frame!=NULL) && (desc->pktlen>JB_FRAME_CONTENT_MAX)){
        printf("JB PutFrame frame=%p, pktlen(%d) > %d\n",
            frame,desc->pktlen,JB_FRAME_CONTENT_MAX);
        return PiRErrTooBig ;
    }

    LogPrint(JBTL_API,LD_MASK_STATUS,
        "PutFrame CALLED desc->seq=%d,len=%d,ts=%u,issp=%d,isdf=%d",
        desc->seqnumber,
        desc->pktlen,
        desc->timestamp,
        desc->isspeech,
        desc->isdtmf);

    cur_size = m_FrameList.EffSize();

    if(m_Config.UseAdjust){
        if(isspeechzone==0){
            // silient zone
            if( (desc->isspeech==1)&&(cur_size >m_Config.MinPrefetch) ){
                // Adjust Buffer Size
                m_FrameList.Adjust(m_Config.MinPrefetch,m_CurIndex); 
                cur_size = m_FrameList.EffSize();
            }
        }
    }

    if(m_Config.UseTsAsIndex){
        //seqnumber =((desc->timestamp/20))%65536 ;
        seqnumber =((desc->timestamp/160))%65536 ;
    }else{
        seqnumber = desc->seqnumber ;
    }

    // sequence diff calculate
    // set index value using FL origin value.
    if(m_PutFrameCount==0){
        // FL reset JB reset status.
        m_PutFrameRtpSeqMax = seqnumber ;
        m_CurIndex          = m_PutFrameRtpSeqMax+1000 ;
        desc->index         = m_CurIndex ;

        m_LastPutFrameRxTime = desc->rxtime_msec ;
        m_LastPutFrameRtpTs  = desc->timestamp ;
        m_LastPutFrameRtpIsSpeech = desc->isspeech ;
    }else{

        delta = (piub16)seqnumber - m_PutFrameRtpSeqMax ;

        if(delta==0){
            // duplicated
            delta = 0 ;
            desc->index = m_CurIndex ;

            duplicated=1;
            m_Stats.SeqDuplicated++;

        } else if(delta < ((piub16)(m_Config.MaxDropout))){
            // forward jump.

            m_CurIndex         += delta ;
            desc->index         = m_CurIndex ;
            m_PutFrameRtpSeqMax = seqnumber ;

            if(delta>1){
                if(m_LastPutFrameRtpIsSpeech){
                    m_Stats.SeqFwJump++;
                    LogPrint(JBTL_INF,"Rtp(seq=%d,ts=%u)Seq Jump %d",
                        desc->seqnumber,desc->timestamp,delta); 
                }
            }

            pisb32 tsmsec =(pisb32)( 
                (piub32)desc->timestamp-(piub32)m_LastPutFrameRtpTs) ;

            pisb32 jittermsec,jitterdelay ;

            //if( tsmsec < 0 || tsmsec > (20*50*2)){
            if( tsmsec < 0 || tsmsec > (160*50*2)){
                m_Stats.JDLevelForgot++;
            }else{

                tsmsec = tsmsec / 8 ;
                jittermsec = (pisb32)((unsigned long)desc->rxtime_msec -(unsigned long) m_LastPutFrameRxTime) ;

                if(jittermsec < 0 || jittermsec > 1000*10){
                    m_Stats.JDLevelForgot++;
                }else{
                    if(tsmsec > jittermsec){
                        jitterdelay = tsmsec - jittermsec ;
                    }else{
                        jitterdelay = jittermsec-tsmsec ;
                    }
                    m_Stats.UpdateJDLevel((piub32)jitterdelay);
                }
            }

            m_LastPutFrameRxTime = desc->rxtime_msec   ;
            m_LastPutFrameRtpTs  = desc->timestamp     ;
            m_LastPutFrameRtpIsSpeech = desc->isspeech ;

        }else if(delta < (RTP_SEQ_MOD - m_Config.MaxMisorder)){
            // forward jump is large
           m_Stats.SeqRestarted++;
           // framelist buffer reset.
           m_FrameList.Reset();

           LogPrint(JBTL_INF,"Rtp Seq Restarted"); 

           m_PutFrameRtpSeqMax = seqnumber ;
           m_CurIndex          = m_PutFrameRtpSeqMax+1000 ;
           desc->index         = m_CurIndex ;
        }else{
            // misorder range.
            // never update m_CurIndex value this case.
#if 0
            desc->index = (m_CurIndex-(int)delta) ;
            misordered=1;
            m_Stats.SeqMisordered++;
#else
            // CAUTION 
            // Not change m_PutFrameRtpSeqMax
            // Not change m_CurIndex.
            desc->index =  m_CurIndex - (RTP_SEQ_MOD-(int)delta) ;
            misordered=1;
            m_Stats.SeqMisordered++;
            LogPrint(JBTL_INF,"Rtp Seq Misordered"); 
#endif
        }
    }



    m_PutFrameCount++;

    result = m_FrameList.PutAt(desc,frame,&reseted);
    // result : 
    //     resshort : overrun occurred
    //     already  : duplicated packet
    //     toolate  : packet too late due to mis order
    //     invalidparam,TooBig  : system failure.
    //     Error    : sysfailure.
    if(result==PiRErrResShortage){
        int      distance;
        unsigned removed ;

        LogPrint(JBTL_INF,
            LD_MASK_CFG|LD_MASK_JB_STATUS|LD_MASK_FL_STATUS,
            "PutFrame res shortage");
        LogPrint(JBTL_DBG,LD_MASK_FL_FRAMES,"res shortage");

        distance = (desc->index - m_FrameList.Origin()) - 
                     m_FrameList.MaxCount()+1;

        if(m_Config.DropsOnResShort){
            LogPrint(JBTL_INF,"DropsOnResShort %d set dist %d -> %d",
                m_Config.DropsOnResShort,
                distance,distance+m_Config.DropsOnResShort);
            distance += m_Config.DropsOnResShort ;
        }

        removed  = m_FrameList.RemoveHead((piub32)distance);
        LogPrint(JBTL_INF,"RemoveHead(%u) return %d",(piub32)distance,removed);

        result = m_FrameList.PutAt(desc,frame,&reseted);
        if(result!=PiSuccess){
            printf("CSID(%d):JB PutFrame : PutAt return %d\n",
               m_Config.CSID,result);
            result = PiRError ; 
        }
    }

    new_size   = m_FrameList.EffSize();
    *odiscarded = (result!=PiSuccess) ;

    if(result == PiSuccess){
        if(misordered){
            m_Stats.SeqMisorderCorrected++;
        }

        if(m_Prefetching){
            if(new_size >= m_Prefetch){
                m_Prefetching = 0 ;
                LogPrint(JBTL_INF,LD_MASK_STATUS,"Prefetching OFF");
            }
        }

        // forward short-jump occurred. new_size > cur_size 
        // forward long-jump. sequence restarted case -> cur_size > new_size
        m_Level += ( new_size > cur_size ? new_size - cur_size : 1) ;

        Update(E_CJBOper_Put);

    }else{
        if(result == PiRErrTooLate){
            m_Stats.SeqMisorderDroped++ ;
#if 1 // jcryu
            if(misordered!=1){
                printf("SEQ check not misorder,jbfl return toolate\n");
            }
#endif 
        }else if(result==PiRErrAlready){
#if 1 // jcryu
            if(duplicated!=1){
                printf("SEQ check not duplicated,jbfl return Already\n");
            }
#endif 
        }

        LogPrint(JBTL_INF,"PutFrame seq=%d,ts=%u,size=%d,idx=%d discarded",
            desc->seqnumber,desc->timestamp,desc->pktlen,desc->index);
    }

    LogPrint(JBTL_API,LD_MASK_STATUS,"PutFrame ended res=%d",result);

    return result ;
}

#if 0
void CJBuffer::PeekFrame(
    piub32 offset,
    JBFrameDesc_T*odesc,
    pibyte*oFrameBuffer,
    piub32 FrameBufferLen){

    PiResult_T      result;

    result = m_FrameList.Peek(offset,odesc,oFrameBuffer,FrameBufferLen);
    if(result!=PiSuccess){
        odesc->frmtype = E_JBFrameType_ZeroEmpty ;
    }else{
    }
}
#endif

PiResult_T CJBuffer::GetFrame(
    JBFrameDesc_T*odesc,
    pibyte*oFrameBuffer,
    piub32 FrameBufferLen){

    PiResult_T      result=PiSuccess ;

    if(odesc==NULL){
        return PiRErrInvalidParam ;
    }

    m_Stats.GetCalled++;

#ifdef _DEBUG_JBUF_
    fprintf(stderr,"GetFrame CALLED\n"); 
#endif    

    if(m_Prefetching){
        odesc->frmtype = E_JBFrameType_ZeroPrefetch ;
        odesc->pktlen  = 0 ;

    }else{

        odesc->frmtype = E_JBFrameType_Normal ;

        result = m_FrameList.Get(odesc,oFrameBuffer,FrameBufferLen);
        if(result==PiSuccess){
            m_Stats.GetSuccess++; 
            if(odesc->frmtype == E_JBFrameType_Normal){
                LogPrint(JBTL_DBG,"GetFrame Normal");
            }else{
                odesc->frmtype = E_JBFrameType_Missing ;
                LogPrint(JBTL_DBG,"GetFrame Missing");
            }
#if 0
            /* Store delay history at the first GET */
            if(m_LastOper==E_CJBOper_Put){
            }
#endif
        }else{
            // TooBig, Again
            if(result==PiRErrTooBig || result==PiRErrInvalidParam){
#ifdef _DEBUG_JBUF_               
                fprintf(stderr,"JB FL GetFrame return %d\n",result);
#endif
                // sys failure.
                m_Stats.GetSysfail++;
            }else if(result==PiRErrAgain){
                // empty state.
                result = PiSuccess ; 

                LogPrint(JBTL_DBG,"GetFrame Jitter Buffer is empty");

                m_Stats.GetErrAgain++;
            }else {
                // unexpected result.
                //LOGGER(PL_CRITICAL,"JB FR Get return %d unexpected",
                    //result);
                result = PiRError ; 
                m_Stats.GetSysfail++;
            }

            // Jitter buffer is empty
            if(m_Prefetch){
                m_Prefetching = 1 ;
                LogPrint(JBTL_INF,LD_MASK_STATUS,"Prefetching ON");
            }

            odesc->frmtype = E_JBFrameType_ZeroEmpty ;
            odesc->pktlen  = 0 ;

            LogPrint(JBTL_DBG,"GetFrame ZeroEmpty : Jitter Buffer is empty");
        }
    }

    m_Level++;

    Update(E_CJBOper_Get);

    LogPrint(JBTL_API,LD_MASK_STATUS,
        "GetFrame ended res=%d,desc->seq=%d,len=%d,ts=%u,issp=%d,isdf=%d",
        result,
        odesc->seqnumber,
        odesc->pktlen,
        odesc->timestamp,
        odesc->isspeech,
        odesc->isdtmf);

    return result ;
}

#if 1
void CJBuffer::LogPrint(piub32 level,char*fmt,...){
    pichar line[1024];
    va_list arg_list    ;

    if((pisb32)level > _g_JBTLogLevel){
        return ;
    }

    va_start(arg_list,fmt);
    vsnprintf(line,1023,fmt,arg_list);
    va_end(arg_list);

    fprintf(stderr,"CSID(%5d):JBLOG:%s\n",m_Config.CSID,line);
}

void CJBuffer::LogPrint(piub32 level,piub32 mask,char*fmt,...){
    pichar line[1024];
    va_list arg_list    ;

    if((pisb32)level > _g_JBTLogLevel){
        return ;
    }

    va_start(arg_list,fmt);
    vsnprintf(line,1023,fmt,arg_list);
    va_end(arg_list);

    fprintf(stderr,"%s\n",line);

    //LogDump(level,mask);
}
#endif

#if 0
void CJBuffer::LogDump(piub32 level,piub32 mask){

    if((pisb32)level > _g_JBTLogLevel){
        return ;
    }

    CLogBufShort logbuf ;

    if(mask&LD_MASK_CFG){
        logbuf.CPrint(3,
"CSID(%5d):JBCFG:MD=%d,MM=%d,FPT=%d,P=(%d,%d,%d),MB=%d,DORS=%d,AUM=%d,UTAI=%d,UA=%d\n",
        m_Config.CSID,
        m_Config.MaxDropout,
        m_Config.MaxMisorder,
        m_Config.FramePTime,
        m_Config.InitPrefetch,
        m_Config.MinPrefetch,
        m_Config.MaxPrefetch,
        m_Config.MaxBurst,
        m_Config.DropsOnResShort,
        m_Config.AllowUnderMin,
        m_Config.UseTsAsIndex,
        m_Config.UseAdjust);
        logbuf.CPrint(3,
"           :FLCFG:MC=%d,MFS=%d,MD=%d,MM=%d\n",
        m_FrameList.m_MaxCount,
        m_FrameList.m_MaxFrameSize,
        m_FrameList.m_MaxDropout,
        m_FrameList.m_MaxMisorder);
    }
    
    if(mask&LD_MASK_JB_STATUS){
        logbuf.CPrint(3,
"CSID(%5d):JBSTS:FL.SIZE=%d,CIDX=%d,P.FET=%d,PFC=%d,PFRSM=%d,S=%d,ICC=%d,P.ING=%d,L=%d,LO=%d,MHL=%d,SH=%d,EL=%d\n",
        m_Config.CSID,
        m_FrameList.m_Size,
        m_CurIndex,
        m_Prefetch,
        m_PutFrameCount,
        m_PutFrameRtpSeqMax,
        m_State,
        m_InitCycleCnt,
        m_Prefetching,
        m_Level,
        m_LastOper,
        m_MaxHistLevel,
        m_StableHist,
        m_EffLevel);
    }
    if(mask&LD_MASK_FL_STATUS){
        logbuf.CPrint(3, 
"           :FLSTS:ORIG=%d,SIZE=%d,HIDX=%d,DISNUM=%d",
        m_FrameList.m_Origin,
        m_FrameList.m_Size,
        m_FrameList.m_HeadIdx,
        m_FrameList.m_DiscardedNum);
    }

    if(mask&LD_MASK_FL_FRAMES){

        piub32 count,pos ;

        logbuf.CPrint(3,"CSID(%5d):FL.FRAMES(%d){",m_Config.CSID,m_FrameList.m_Size);

        for(count = 0 ; count < m_FrameList.m_Size; count++){
            pos = (m_FrameList.m_HeadIdx+count)%m_FrameList.m_MaxCount ; 
            logbuf.CPrint(1,"[i=%d,ft=%d,len=%d,seq=%d,ts=%u,issp=%d]",
                pos,
                m_FrameList.m_Array[pos].type,
                m_FrameList.m_Array[pos].contentlen,
                m_FrameList.m_Array[pos].seqnum,
                m_FrameList.m_Array[pos].timestamp,
                m_FrameList.m_Array[pos].isspeech);
        }

        for( ; count < m_FrameList.m_MaxCount ; count++){
            pos = (m_FrameList.m_HeadIdx+count)%m_FrameList.m_MaxCount ; 
            logbuf.CPrint(1,"+i=%d,ft=%d,len=%d,seq=%d,ts=%u,issp=%d+",
                pos,
                m_FrameList.m_Array[pos].type,
                m_FrameList.m_Array[pos].contentlen,
                m_FrameList.m_Array[pos].seqnum,
                m_FrameList.m_Array[pos].timestamp,
                m_FrameList.m_Array[pos].isspeech);
        }
    }

    if(mask&LD_MASK_JB_STATS){

    }

    if(mask&LD_MASK_FL_STATS){

    }
    fprintf(stderr,"%s",logbuf.GetBuffer());
}

void CJBuffer::LoggingStats(pisb32 level){

    //LOGGER(PL_INFO,"LoggingStats Level = %d",level);

    if(level<=1){
        return  ;
    }

    CLogBufBig logbuf ;

    logbuf.CPrint(1,"CSID(%5d):JBSTATS:SEQ(RX=%d,RE=%d,FWJ=%d,DUP=%d,MIS=%d,MISC=%d,MISD=%d,DISCARD=%d)",
        m_Config.CSID,
        m_Stats.SeqReceived,
        m_Stats.SeqRestarted,
        m_Stats.SeqFwJump,
        m_Stats.SeqDuplicated,
        m_Stats.SeqMisordered,
        m_Stats.SeqMisorderCorrected,
        m_Stats.SeqMisorderDroped,
        m_Stats.SeqDiscarded);

    logbuf.CPrint(1,":Get(CALLED=%d,SUCC=%d,EAG=%d,SYSFAIL=%d):D:JDLO=%d,JDLF=%d,JDHM=%d,GDLO=%d,GDHM=%d",
        m_Stats.GetCalled,
        m_Stats.GetSuccess,
        m_Stats.GetErrAgain,
        m_Stats.GetSysfail,
        m_Stats.JDLevelOver,
        m_Stats.JDLevelForgot,
        m_Stats.JDHighMark,
        m_Stats.GetDriftLevelOver,
        m_Stats.GetDriftHighMark);

    logbuf.CPrint(1,":FLSTATS:Put(TLSID=%d,TLS=%d,RSSID=%d,RSS=%d,DUPSID=%d,DUPS=%d,SID=%d,S=%d)",
        m_FrameList.m_Stats.PutTooLateSID,
        m_FrameList.m_Stats.PutTooLateSpeech,
        m_FrameList.m_Stats.PutResShortSID,
        m_FrameList.m_Stats.PutResShortSpeech,
        m_FrameList.m_Stats.PutDupSID, 
        m_FrameList.m_Stats.PutDupSpeech,
        m_FrameList.m_Stats.PutSID,
        m_FrameList.m_Stats.PutSpeech);

    logbuf.CPrint(1,":RM(S=%d,SID=%d,MIS=%d,DIS=%d)",
        m_FrameList.m_Stats.RemoveSpeech,
        m_FrameList.m_Stats.RemoveSID,
        m_FrameList.m_Stats.RemoveMissing,
        m_FrameList.m_Stats.RemoveDiscarded);

    logbuf.CPrint(1,":GET(S=%d,SID=%d,MIS=%d,ZE=%d)",
        m_FrameList.m_Stats.GetSpeech,
        m_FrameList.m_Stats.GetSID,
        m_FrameList.m_Stats.GetMissing,
        m_FrameList.m_Stats.GetZeroEmpty);

    logbuf.CPrint(1,":RST(BW=%d,FW=%d,ORZ=%d,DROP_S=%d,DROP_SID=%d)",
        m_FrameList.m_Stats.ResetBackward,
        m_FrameList.m_Stats.ResetForward,
        m_FrameList.m_Stats.OriginResetZeroSize,
        m_FrameList.m_Stats.ResetDropSpeech,
        m_FrameList.m_Stats.ResetDropSID);
   
    //LOGGER(PL_INFO,"%s",logbuf.GetBuffer());

    logbuf.Reset();

//  if(level>=2){
        int i ;

        logbuf.CPrint(1,"CSID(%5d):JD(",m_Config.CSID);
        for(i=0 ; i < 10 ; i++){
            logbuf.CPrint(0,"L1_%d=%d,",i,m_Stats.JDLevel1[i]);
        }
        for(i = 0 ; i < 10 ; i++){
            logbuf.CPrint(0,"L2_%d=%d,",i,m_Stats.JDLevel2[i]);
        }
        logbuf.CPrint(1,"):GD(");
        for(i=0 ; i < 10 ; i++){
            logbuf.CPrint(0,"L_%d=%d,",i,m_Stats.GetDriftLevel[i]);
        }
//  }

    //LOGGER(PL_INFO,"%s",logbuf.GetBuffer());
}
#endif

void CJBuffer::GetReport(JBReport_T*oreport){

    memcpy(&oreport->JBFLStats,
        &m_FrameList.m_Stats,sizeof(JBFLStatsEnt_T));
    oreport->JBFLStatus.CSID           = m_FrameList.CSID ;
    oreport->JBFLStatus.m_MaxCount     = m_FrameList.m_MaxCount ;
    oreport->JBFLStatus.m_MaxFrameSize = m_FrameList.m_MaxFrameSize ;
    oreport->JBFLStatus.m_MaxDropout   = m_FrameList.m_MaxDropout ;
    oreport->JBFLStatus.m_MaxMisorder  = m_FrameList.m_MaxMisorder ;
    oreport->JBFLStatus.m_Origin       = m_FrameList.m_Origin ;
    oreport->JBFLStatus.m_HeadIdx      = m_FrameList.m_HeadIdx ;
    oreport->JBFLStatus.m_Size         = m_FrameList.m_Size ;
    oreport->JBFLStatus.m_DiscardedNum = m_FrameList.m_DiscardedNum ;

    memcpy(&oreport->JBStats,
        &m_Stats,sizeof(JBStatsEnt_T));
    oreport->JBStatus.m_PutFrameCount     = m_PutFrameCount ;
    oreport->JBStatus.m_PutFrameRtpSeqMax = m_PutFrameRtpSeqMax ;
    oreport->JBStatus.m_LastPutFrameRxTime= m_LastPutFrameRxTime;
    oreport->JBStatus.m_LastPutFrameRtpTs = m_LastPutFrameRtpTs;
    oreport->JBStatus.m_LastPutFrameRtpIsSpeech=m_LastPutFrameRtpIsSpeech;
    oreport->JBStatus.m_State             = m_State;
    oreport->JBStatus.m_InitCycleCnt      = m_InitCycleCnt;
    oreport->JBStatus.m_Prefetching       = m_Prefetching;
    oreport->JBStatus.m_Level             = m_Level;
    oreport->JBStatus.m_LastOper          = m_LastOper;
    oreport->JBStatus.m_MaxHistLevel      = m_MaxHistLevel;
    oreport->JBStatus.m_StableHist        = m_StableHist;
    oreport->JBStatus.m_EffLevel          = m_EffLevel;
    oreport->JBStatus.m_Prefetch          = m_Prefetch;
    oreport->JBStatus.m_CurIndex          = m_CurIndex ;
}

#if 0
void JBZoneTrace_T::LogPrint(piub32 level,char*fmt,...){
    pichar line[1024];
    va_list arg_list    ;

    if((pisb32)level > _g_JBTLogLevel){
        return ;
    }

    va_start(arg_list,fmt);
    vsnprintf(line,1023,fmt,arg_list);
    va_end(arg_list);

    LOGGER(PL_INFO,"CSID(%5d):ZTLOG:%s",CSID,line);
}
#endif

void JBZoneTrace_T::Update(pibyte speech){

        unsigned long nowms = GetNowTimestampMS();
        // 1: speech, 0 : missing, sid
        if(IsSpeechZone){
            // now speech zone
            if(speech){
                // speech frame
                LatestSpeechPlayMSec = nowms ;
            }else{
                // missing, silience frame
                if((nowms - LatestSpeechPlayMSec)>TalkSepMSec){
                    // start silience zone
                    IsSpeechZone  = 0 ;
                    ZoneStartMSec = nowms ;
                    SilientZoneCount++;
                    //LogPrint(JBTL_INF,"Speech Ended");
                }
            }
        }else{
            // now silience zone
            if(speech){
                // start speech zone
                IsSpeechZone  = 1 ;

                LatestSpeechPlayMSec = nowms ;

                ZoneStartMSec = nowms ;
                SpeechZoneCount++;

                //LogPrint(JBTL_INF,"Speech Started");
            }else{
                // now silience zone
            }
        }
    }

} // endof PICO

