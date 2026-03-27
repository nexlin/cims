#include "hudtmf.h"
#include "math.h"
#include <stdio.h>
#define MAX_POWER             ((double)0x7FFF)

#define MAX_DTMF_TONES    29

/*

DTMF keypad frequencies (with sound clips)  
         1209Hz   1336Hz   1477Hz   1633 Hz 
697 Hz   1        2        3        A 
770 Hz   4        5        6        B 
852 Hz   7        8        9        C 
941 Hz   *        0        #        D 

Event Low frequency High frequency 
Busy signal          480 Hz 620 Hz 
Dial tone            350 Hz 440 Hz 
Ringback tone (US)   440 Hz 480 Hz 




-가청 신호음.
발신음(DIAL) : 350+440, continuous, -10 ±5
호출음(RINGBACK) : 440+480, 1 on ~ 2 off, -15 ±5
화중음(BUSY) : 480+620, 0.5 on ~ 0.5 off, -20 ±5
폭주음(CONGESTION) : 480+620, 0.3 on ~ 0.2 off, -20 ±5
대기음(WAITING) : 350+440, 0.25 on ~ 0.25 off ~ 0.25 on ~ 3.25 off, -10 ±5
보류음(HOLDING) : 440+480 350+440, 0.5 on ~ 0.5 off ~ 0.5 on ~ 2.5 off, -10 ±5
가로채기음 : 350+440, 0.5 on ~ 0.25 off ~ 0.125 on ~ 1.5 off, -10 ±5
특수발신음(SPECIAL DIAL) : 392/494/587 350+440, "0.5 on, 0.5 on, 1.5 on" x 2, DIAL-TONE, -10 ±5
확인음 : 392/494/587, 0.5 on ~ 0.5 on ~ 1.5 on, -10 ±5
송수화기방치음(HOWLER) : 1400+2060+2450+2600, 0.1 on ~ 0.1 off, 0 ±2

*Special Information Tone.
Short = 276 ms, Long = 380 ms 
{913.8 Hz, 985.2 Hz}, {1370.6 Hz, 1428.5 Hz}, {1776.7 Hz, N/A}

Reorder (RO1) {Short, Long, Long} {low, high, low} : Incomplete digits, internal office or feature failure - local office
Vacant Code (VC) : {Long, Short, Long} {high, low, low} : Unassigned N11 code, CLASS code or prefix 
No Circuit (NC1) : {Long, Long, Long} {high, high, low} : All circuits busy - local office 
Intercept (IC) : {Short, Short, Long} {low, low, low} : Number changed or disconnected 
Reorder (RO2) : {Short, Long, Long} {high, low, low} : Call failure, no wink or partial digits received - distant office 
No Circuit (NC2) : {Long, Long, Long} {low, low, low} : All circuits busy - distant office 
Ineffective/Other (IO) : {Long, Short, Long} {low, high, low} : General misdialing, coin deposit required or other failure 


*/






static const int s_nLowFreq[MAX_DTMF_TONES] = {
        941,                     //   0
   697, 697, 697,                // 1 2 3 
   770, 770, 770,                // 4 5 6
   852, 852, 852,                // 7 8 9
   941,      941, 
   697, 770, 852, 941,           // A B C D
   700, 900, 1100, 1300, 1500,
   2400, 2600, 2400,
   350, 440, 480,
   0, 0
};

static const int s_nHighFreq[MAX_DTMF_TONES] = {
         1336,                   //   0
   1209, 1336, 1477,             // 1 2 3 
   1209, 1336, 1477,             // 4 5 6
   1209, 1336, 1477,             // 7 8 9
   1209,       1477, 
   1633, 1633, 1633, 1633,       // A B C D
   1700, 1700, 1700, 1700, 1700, //
   0,    0,    2600,
   440, 480, 620,
   0, 0
};


//발신음(DIAL) : 350+440, continuous, -10 ±5
static CHuDtmfGenerator::ATone ToneDial[] = 
   {{350,440,0,0,-1},{-1,}};

//호출음(RINGBACK) : 440+480, 1 on ~ 2 off, -15 ±5
static CHuDtmfGenerator::ATone ToneRingback[] = 
   {{440,480,0,0,1000},{0,0,0,0,2000},{-1,}};

//화중음(BUSY) : 480+620, 0.5 on ~ 0.5 off, -20 ±5
static CHuDtmfGenerator::ATone ToneBusy[] = 
   {{480,620,0,0,500},{0,0,0,0,500},{-1,}};

//폭주음(CONGESTION) : 480+620, 0.3 on ~ 0.2 off, -20 ±5
static CHuDtmfGenerator::ATone ToneCongestion[] = 
   {{480,620,0,0,300},{0,0,0,0,200},{-1,}};

//대기음(WAITING) : 350+440, 0.25 on ~ 0.25 off ~ 0.25 on ~ 3.25 off, -10 ±5
static CHuDtmfGenerator::ATone ToneWaiting[] = 
   {{350,440,0,0,250},{0,0,0,0,250},{350,440,0,0,250},{0,0,0,0,3250},{-1,}};

//보류음(HOLDING) : 440+480, 350+440, 0.5 on ~ 0.5 off ~ 0.5 on ~ 2.5 off, -10 ±5
static CHuDtmfGenerator::ATone ToneHolding[] = 
   {{440,480,0,0,500},{0,0,0,0,500},{350,440,0,0,500},{0,0,0,0,2500},{-1,}};

//가로채기음(INTERCEPT) : 350+440, 0.5 on ~ 0.25 off ~ 0.125 on ~ 1.5 off, -10 ±5
static CHuDtmfGenerator::ATone ToneIntercept[] = 
   {{350,440,0,0,500},{0,0,0,0,250},{350,440,0,0,125},{0,0,0,0,1500},{-1,}};

//특수발신음(SPECIAL DIAL) : 392/494/587, 350+440, "0.5 on, 0.5 on, 1.5 on" x 2, DIAL-TONE, -10 ±5
static CHuDtmfGenerator::ATone ToneSpecialDial[] = 
   {{392,0,0,0,500},{494,0,0,0,500},{587,0,0,0,1500},
   {392,0,0,0,500},{494,0,0,0,500},{587,0,0,0,1500},
   {350,440,0,0,-1},{-1,} };

//확인음(CONFIRM) : 392/494/587, 0.5 on ~ 0.5 on ~ 1.5 on, -10 ±5
static CHuDtmfGenerator::ATone ToneConfirm[] = 
   {{392,0,0,0,500},{494,0,0,0,500},{587,0,0,0,1500},{-1,}};

//송수화기방치음(HOWLER) : 1400+2060+2450+2600, 0.1 on ~ 0.1 off, 0 ±2
static CHuDtmfGenerator::ATone ToneHowler[] = 
   {{1400,2060,2450,2600,100},{0,0,0,0,100},{-1,}};

//*Special Information Tone.
//Short = 276 ms, Long = 380 ms 
//{913.8 Hz, 985.2 Hz}, {1370.6 Hz, 1428.5 Hz}, {1776.7 Hz, N/A}
//{{913.8,985.2,0,0,276/380},{1370.6,1428.5,0,0,276/380},{1776,0,0,0,276/380},{-1,}};

//Reorder (RO1)          : {Short, Long,  Long} {low,  high, low} : Incomplete digits, internal office or feature failure - local office
static CHuDtmfGenerator::ATone ToneSITReorder1[] = 
   {{0,985.2,0,0,276},{0,1428.5,0,0,380},{1776,0,0,0,380},{-1,}};

//Vacant Code (VC)       : {Long,  Short, Long} {high, low,  low} : Unassigned N11 code, CLASS code or prefix 
static CHuDtmfGenerator::ATone ToneSITVacantCode[] = 
   {{0,985.2,0,0,380},{1370.6,0,0,0,276},{1776,0,0,0,380},{-1,}};

//No Circuit (NC1)       : {Long,  Long,  Long} {high, high, low} : All circuits busy - local office 
static CHuDtmfGenerator::ATone ToneSITNoCircuit1[] = 
   {{0,985.2,0,0,380},{0,1428.5,0,0,380},{1776,0,0,0,380},{-1,}};

//Intercept (IC)         : {Short, Short, Long} {low,  low,  low} : Number changed or disconnected 
static CHuDtmfGenerator::ATone ToneSITIntercept[] = 
   {{913.8,0,0,0,276},{1370.6,0,0,0,276},{1776,0,0,0,380},{-1,}};

//Reorder (RO2)          : {Short, Long,  Long} {high, low,  low} : Call failure, no wink or partial digits received - distant office 
static CHuDtmfGenerator::ATone ToneSITReorder2[] = 
   {{0,985.2,0,0,276},{1370.6,0,0,0,380},{1776,0,0,0,380},{-1,}};

//No Circuit (NC2)       : {Long,  Long,  Long} {low,  low,  low} : All circuits busy - distant office 
static CHuDtmfGenerator::ATone ToneSITNoCircuit2[] = 
   {{913.8,0,0,0,380},{1370.6,0,0,0,380},{1776,0,0,0,380},{-1,}};

//Ineffective/Other (IO) : {Long,  Short, Long} {low,  high, low} : General misdialing, coin deposit required or other failure 
static CHuDtmfGenerator::ATone ToneSITIneffectiveOther[] = 
   {{913.8,0,0,0,380},{0,1428.5,0,0,276},{1776,0,0,0,380},{-1,}};


/* --------------------------------------------------------------- */

#define D_PI 3.1415926535897932384626433832795029

//#include <fcntl.h>
// take the sine of x, where x is 0 to 65535 (for 0 to 360 degrees)


#if 1

static inline double Sine(short in)
{
   return sin(in/(double)0x7FFF * D_PI);
}
#else
static double Sine(short in)
{
   static double coef[] = {
      3.140625f, 0.02026367f, -5.325196f, 0.5446778f, 1.800293f };
   double x,y,res;
   int sign,i;

   if(in < 0) {       /* force positive */
      sign = -1;
      in = -in;
   } else {
      sign = 1;
   }
   if(in >= 0x4000) {     /* 90 degrees */
      in = 0x8000 - in;   /* 180 degrees - in */
   }
   x = in * (1/32768.0); 
   y = x;               /* y holds x^i) */
   res = 0;
   for(i=0; i<5; i++) {
      res += y * coef[i];
      y *= x;
   }
   return(res * sign); 
}
#endif


CHuDtmfGenerator::CHuDtmfGenerator()
{
   m_bOpened = false;
   m_bSet = false;
   m_nSampleRate = 0;
   m_dMaxPower = 0;
}
CHuDtmfGenerator::~CHuDtmfGenerator()
{

}

int CHuDtmfGenerator::Open(int nSampleRate, int nDelay)
{
   if(m_bOpened || m_bSet) return -1;
   m_nSampleRate = nSampleRate;
   m_nDelaySamples = (m_nSampleRate * 2)/1000 * nDelay;

   m_bOpened = true;
   return 0;
}

int CHuDtmfGenerator::Close()
{
   m_bOpened = false;
   m_bSet = false;
   return 0;
}

int CHuDtmfGenerator::Set(DTMF_CODE nDtmfCode, int nDuration, int nPower)
{
   if(m_bSet) return -1;
   if((nDtmfCode < 0) || (nDtmfCode == DTMF_NONE) || (nDtmfCode >= TONE_END)) {
      return -2; // invalid code.
   }

   m_dMaxPower = MAX_POWER * (nPower / 100.0);
   m_nSampleLeft = int (((double)nDuration * m_nSampleRate) / 1000 + m_nDelaySamples);
   m_nDelaySampleLeft = m_nDelaySamples;
   m_nLastTone = nDtmfCode;

   if(nDtmfCode < DTMF_NONE) {
      m_bIsDtmf = true;
      m_uRadOfLowFreq = m_uRadOfHighFreq = 0;
      m_uRadPerSampleOfLowFreq  = (s_nLowFreq[nDtmfCode] << 16) / m_nSampleRate;
      m_uRadPerSampleOfHighFreq = (s_nHighFreq[nDtmfCode] << 16) / m_nSampleRate;
      m_pToneList = NULL;
   } else {
      m_bIsDtmf = false;
      switch(nDtmfCode) {
         case TONE_DIAL        : m_pToneList = ToneDial                 ; break;
         case TONE_RINGBACK    : m_pToneList = ToneRingback             ; break;
         case TONE_BUSY        : m_pToneList = ToneBusy                 ; break;
         case TONE_CONGESTION  : m_pToneList = ToneCongestion           ; break;
         case TONE_WAITING     : m_pToneList = ToneWaiting              ; break;
         case TONE_HOLDING     : m_pToneList = ToneHolding              ; break;
         case TONE_INTERCEPT   : m_pToneList = ToneIntercept            ; break;
         case TONE_SPECIALDIAL : m_pToneList = ToneSpecialDial          ; break;
         case TONE_CONFIRM     : m_pToneList = ToneConfirm              ; break;
         case TONE_HOLWER      : m_pToneList = ToneHowler               ; break;
         case TONE_SIT_RO1     : m_pToneList = ToneSITReorder1          ; break;
         case TONE_SIT_VC      : m_pToneList = ToneSITVacantCode        ; break;
         case TONE_SIT_NC1     : m_pToneList = ToneSITNoCircuit1        ; break;
         case TONE_SIT_IC      : m_pToneList = ToneSITIntercept         ; break;
         case TONE_SIT_RO2     : m_pToneList = ToneSITReorder2          ; break;
         case TONE_SIT_NC2     : m_pToneList = ToneSITNoCircuit2        ; break;
         case TONE_SIT_IO      : m_pToneList = ToneSITIneffectiveOther  ; break;
         default: return -3;
      }
   }
   m_bSet = true;
   m_nEncSamples = 0;

   return 0;
}

int CHuDtmfGenerator::Encode(unsigned char * pFrame, int nFrameSize, bool bMix)
{
   if(!m_bSet) {
      return 0;
   }
   if(m_bIsDtmf) {
      return EncodeDtmf(pFrame, nFrameSize, bMix);
   } else {
      return EncodeTone(pFrame, nFrameSize, bMix);
   }
   return 0;
}

int CHuDtmfGenerator::EncodeDtmf(unsigned char * pFrame, int nFrameSize, bool bMix)
{
   int i;

   short * pSample = (short *)pFrame;
   int nSampleCount = nFrameSize / sizeof(short);
   if(m_nDelaySampleLeft > nSampleCount) {
      m_nDelaySampleLeft -= nSampleCount;
      nSampleCount = 0;
   } else {
      nSampleCount -= m_nDelaySampleLeft;
      m_nDelaySampleLeft = 0;
      pSample += m_nDelaySampleLeft;
   }

   if(nSampleCount > m_nSampleLeft) {
      nSampleCount = m_nSampleLeft;
      m_nSampleLeft = 0;
   } else {
      m_nSampleLeft -= nSampleCount;
   }
   for(i=0; i<nSampleCount; i++, pSample++) {
      if(bMix) {
         *pSample = (short) ( 
            (Sine(m_uRadOfLowFreq) + Sine(m_uRadOfHighFreq)) * 0.5 * m_dMaxPower * 0.5 +
            (*pSample * 0.5)
            );
      } else {
         *pSample = (short) (
            (Sine(m_uRadOfLowFreq) + Sine(m_uRadOfHighFreq)) * 0.5 * m_dMaxPower
            );
      }
      m_uRadOfLowFreq  += m_uRadPerSampleOfLowFreq;
      m_uRadOfHighFreq += m_uRadPerSampleOfHighFreq;
   }
   
   if(m_nSampleLeft <= 0) {
      m_bSet = false; // Clear
   }
   return m_nSampleLeft*sizeof(short);
}

int  CHuDtmfGenerator::CalcFrameLen(int nSampleRate, int nDuration)
{
   return sizeof(short) * (int)(nDuration/1000.0 * nSampleRate);
}

int CHuDtmfGenerator::MakeTone(unsigned char * pFrame, int nFrameLen, 
                               int nSampleRate, DTMF_CODE nDtmfCode, int nPower)
{
   int rc;
   rc = Open(nSampleRate, 0);
   if(rc != 0) return rc;
   rc = Set(nDtmfCode, nFrameLen, nPower);
   if(rc != 0) return rc;
   rc = Encode(pFrame, nFrameLen);
   int n = nFrameLen - rc;
   rc = Close();
   return n;
}




int CHuDtmfGenerator::EncodeTone(unsigned char * pFrame, int nFrameSize, bool bMix)
{
   int i;
   if(!m_bSet) {
      return 0;
   }

   short * pSample = (short *)pFrame;
   int nSampleCount = nFrameSize / sizeof(short);
   if(m_nDelaySampleLeft > nSampleCount) {
      m_nDelaySampleLeft -= nSampleCount;
      nSampleCount = 0;
   } else {
      nSampleCount -= m_nDelaySampleLeft;
      m_nDelaySampleLeft = 0;
      pSample += m_nDelaySampleLeft;
   }

   if(nSampleCount > m_nSampleLeft) {
      nSampleCount = m_nSampleLeft;
      m_nSampleLeft = 0;
   } else {
      m_nSampleLeft -= nSampleCount;
   }
   double f1, f2, f3, f4, avg;
   int nTimeElapse, nTimeAccm, nIndex, nToneCount, nFreqCount;
   for(nToneCount=0;;nToneCount++) {
      if(m_pToneList[nToneCount].f1 < 0) break;
   }
   nTimeAccm = 0;
   nIndex = -1;

   for(i=0; i<nSampleCount; i++) {
      nTimeElapse = 1000 * m_nEncSamples / m_nSampleRate;
      while(nTimeElapse >= nTimeAccm) {
         nIndex = (nIndex + 1) % nToneCount;
         nTimeAccm += (m_pToneList[nIndex].tm < 0) ? 0x7FFFFFFF : m_pToneList[nIndex].tm;
         f1 = m_pToneList[nIndex].f1;
         f2 = m_pToneList[nIndex].f2;
         f3 = m_pToneList[nIndex].f3;
         f4 = m_pToneList[nIndex].f4;
         nFreqCount = ((f1==0)?0:1) + ((f2==0)?0:1) + ((f3==0)?0:1) + ((f4==0)?0:1);
      }
      avg = ( ( (f1 == 0) ? 0 : sin(2 * D_PI * f1 / m_nSampleRate * m_nEncSamples) ) +
              ( (f2 == 0) ? 0 : sin(2 * D_PI * f2 / m_nSampleRate * m_nEncSamples) ) +
              ( (f3 == 0) ? 0 : sin(2 * D_PI * f3 / m_nSampleRate * m_nEncSamples) ) +
              ( (f4 == 0) ? 0 : sin(2 * D_PI * f4 / m_nSampleRate * m_nEncSamples) )
            ) / nFreqCount;
      if(bMix) {
         *pSample = (short) ( avg * m_dMaxPower + (*pSample) ) / 2;
      } else {
         *pSample = (short) ( avg * m_dMaxPower );
      }
      m_uRadOfLowFreq  += m_uRadPerSampleOfLowFreq;
      m_uRadOfHighFreq += m_uRadPerSampleOfHighFreq;
      pSample++;
      m_nEncSamples++;
   }
   
   if(m_nSampleLeft <= 0) {
      m_bSet = false; // Clear
   }
   return m_nSampleLeft*sizeof(short);
}



