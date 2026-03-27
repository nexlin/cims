/* 
 * goertzel aglorithm, find the power of different
 * frequencies in an N point DFT.
 *
 * ftone/fsample = k/N   
 * k and N are integers.  fsample is 8000 (8khz)
 * this means the *maximum* frequency resolution
 * is fsample/N (each step in k corresponds to a
 * step of fsample/N hz in ftone)
 *
 * N was chosen to minimize the sum of the K errors for
 * all the tones detected...  here are the results :
 *
 * Best N is 240, with the sum of all errors = 3.030002
 * freq  freq actual   k     kactual  kerr
 * ---- ------------  ------ ------- -----
 *  350 (366.66667)   10.500 (11)    0.500
 *  440 (433.33333)   13.200 (13)    0.200
 *  480 (466.66667)   14.400 (14)    0.400
 *  620 (633.33333)   18.600 (19)    0.400
 *  697 (700.00000)   20.910 (21)    0.090
 *  700 (700.00000)   21.000 (21)    0.000
 *  770 (766.66667)   23.100 (23)    0.100
 *  852 (866.66667)   25.560 (26)    0.440
 *  900 (900.00000)   27.000 (27)    0.000
 *  941 (933.33333)   28.230 (28)    0.230
 * 1100 (1100.00000)  33.000 (33)    0.000
 * 1209 (1200.00000)  36.270 (36)    0.270
 * 1300 (1300.00000)  39.000 (39)    0.000
 * 1336 (1333.33333)  40.080 (40)    0.080
 **** I took out 1477.. too close to 1500
 * 1477 (1466.66667)  44.310 (44)    0.310
 ****
 * 1500 (1500.00000)  45.000 (45)    0.000
 * 1633 (1633.33333)  48.990 (49)    0.010
 * 1700 (1700.00000)  51.000 (51)    0.000
 * 2400 (2400.00000)  72.000 (72)    0.000
 * 2600 (2600.00000)  78.000 (78)    0.000
 *
 * notice, 697 and 700hz are indestinguishable (same K)
 * all other tones have a seperate k value.  
 * these two tones must be treated as identical for our
 * analysis.
 *
 * The worst tones to detect are 350 (error = 0.5, 
 * detet 367 hz) and 852 (error = 0.44, detect 867hz). 
 * all others are very close.
 *
 * This program will detect MF tones and normal
 * dtmf tones as well as some other common tones such
 * as BUSY, DIALTONE and RING.
 * The program uses a goertzel algorithm to detect
 * the power of various frequency ranges.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include "hudtmf.h"

//#define PEAK_RANGE    0.28       /* any thing higher than RANGE*peak is "on" */
#define PEAK_RANGE    0.18       /* any thing higher than RANGE*peak is "on" */
//#define PEAK_RANGE    0.10       /* any thing higher than RANGE*peak is "on" */
#define POWER_THRESH  100.0     /* minimum level for the loudest tone */
#define DEFAULT_POWER_THRESH  10.0     /* minimum level for the loudest tone */
#define FLUSH_TIME    100       /* 100 frames = 3 seconds */

#define X1    0    /* 350 dialtone */
#define X2    1    /* 440 ring, dialtone */
#define X3    2    /* 480 ring, busy */
#define X4    3    /* 620 busy */

#define R1    4    /* 697, dtmf row 1 */
#define R2    5    /* 770, dtmf row 2 */
#define R3    6    /* 852, dtmf row 3 */
#define R4    8    /* 941, dtmf row 4 */
#define C1   10    /* 1209, dtmf col 1 */
#define C2   12    /* 1336, dtmf col 2 */
#define C3   13    /* 1477, dtmf col 3 */
#define C4   14    /* 1633, dtmf col 4 */

#define B1    4    /* 700, blue box 1 */
#define B2    7    /* 900, bb 2 */
#define B3    9    /* 1100, bb 3 */
#define B4   11    /* 1300, bb4 */
#define B5   13    /* 1500, bb5 */
#define B6   15    /* 1700, bb6 */
#define B7   16    /* 2400, bb7 */
#define B8   17    /* 2600, bb8 */

#if 0
#define DTMF_DIFF_TIME(tvp,uvp) \
   ((tvp.tv_sec - uvp.tv_sec)*1000 + \
   (tvp.tv_usec - uvp.tv_usec)/1000)
#endif
/* values returned by detect 
 *  0-9     DTMF 0 through 9 or MF 0-9
 *  10-11   DTMF *, #
 *  12-15   DTMF A,B,C,D
 *  16-20   MF last column: C11, C12, KP1, KP2, ST
 *  21      2400
 *  22      2600
 *  23      2400 + 2600
 *  24      DIALTONE
 *  25      RING
 *  26      BUSY
 *  27      silence
 *  -1      invalid
 */

#define NUMTONES 18 

/*
 * calculate the power of each tone according
 * to a modified goertzel algorithm described in
 *  _digital signal processing applications using the
 *  ADSP-2100 family_ by Analog Devices
 * input is 'data',  N sample values
 * ouput is 'power', NUMTONES values
 *  corresponding to the power of each tone 
 */

#if 0//KKK redrock
static char* getFormattedTime(char * buffer) {
    // For Miliseconds
    int millisec;
    struct tm* tm_info;
    struct timeval tv;

    // For Time
    time_t rawtime;
    struct tm* timeinfo;

    gettimeofday(&tv, NULL);

    millisec = lrint(tv.tv_usec / 1000.0);
    if (millisec >= 1000) 
    {
        millisec -= 1000;
        tv.tv_sec++;
    }

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, 26, "%Y:%m:%d %H:%M:%S", timeinfo);
    sprintf(buffer,"%s.%03d", buffer, millisec);

    return buffer;
}
#endif


static int freqs[NUMTONES] =  {
       350, 440, 480, 620, 697, 770, 852, 900, 941,1100,
       1209,1300,1336,1477,1633,1700,2400, 2600 };

CHuDtmfDetector::CHuDtmfDetector()
{
   m_bOpened = false;
   m_pCoef = new float[NUMTONES];
   m_pSample = NULL;
   m_nSampleLeft = 0;
   m_nLastTone = DTMF_SIL;
   m_nDtmfDuration = 0;
   // added by wjstkddn92 20201016 vms dtmf detect issue
   m_nMFmode = 0;
}

CHuDtmfDetector::~CHuDtmfDetector()
{
   if(m_pCoef) delete[] m_pCoef;
   if(m_pSample) {
      delete[] m_pSample;
      m_pSample = NULL;
   }
}

int CHuDtmfDetector::Close()
{
   if(!m_bOpened) return -1;
   if(m_pSample) delete [] m_pSample;
   m_pSample = NULL;

   // added by wjstkddn92 20201016 vms dtmf detect issue
   m_nMFmode = 0;

   m_nSampleLeft = 0;
   m_bOpened = false;
   return 0;
}

int CHuDtmfDetector::Open(int nSampleRate, int nTimeWinSize, int nMinDuration, int nPowerThresh, bool bDetectOnce)
{
   if(m_bOpened) return -1;
   
   if(nSampleRate <= 0) {
      return -1;
   }
   if(nSampleRate > 96000) {
      return -1;
   }

   m_nSampleRate = nSampleRate;

   m_nTimeWinSize = nTimeWinSize;
   if(m_nTimeWinSize <= 0) {
      m_nTimeWinSize = 30;
   }
   m_nMinDuration = nMinDuration;
   if(m_nMinDuration <= 0) {
      m_nMinDuration = 30;
   }
   m_dPowerThresh = (double)nPowerThresh;
   if(m_dPowerThresh <= 0 || m_dPowerThresh > 10000) {
      m_dPowerThresh = DEFAULT_POWER_THRESH;
   }
   m_bDetectOnce = bDetectOnce;


   m_nSampleWinSize = (int)((float)m_nTimeWinSize * m_nSampleRate / 1000.0) * sizeof(short);
   m_pSample = new unsigned char[m_nSampleWinSize];

   m_nLastTone = DTMF_SIL;
   m_nDtmfDuration = 0;
   m_nSampleLeft = 0;

   CalcCoef();

   // added by wjstkddn92 20201016 vms dtmf detect issue
   m_nMFmode = 0;

   m_bOpened = true;
   return 0;
}

int CHuDtmfDetector::CalcCoef()
{
   static float pi = 3.1415926f;
   for(int i=0;i<NUMTONES;i++) {
#if 0
      int k = (int) (0.5 + ((float)m_nSampleWinSize / sizeof(short) * freqs[i]) / m_nSampleRate);
      m_pCoef[i] = (float)( 2.0 * cos(2.0 * pi * k / (m_nSampleWinSize/sizeof(short))) );
#else
      m_pCoef[i] = (float)( 2.0 * cos(2.0 * pi * freqs[i] / m_nSampleRate) );
#endif
   }
   return 0;
}


int CHuDtmfDetector::CalcPower(short *data, double *power, int nInLen)
{
   float u0[NUMTONES],u1[NUMTONES],t,in;
   int i,j;

   for(j=0; j<NUMTONES; j++) {
      u0[j] = 0.0;
      u1[j] = 0.0;
   }
   for(i=0; i<nInLen; i++) {   /* feedback */
      //in = (float)(data[i] / 32768.0);
      in = (float)(data[i] / 32768.0);
      for(j=0; j<NUMTONES; j++) {
         t = u0[j];
         u0[j] = in + m_pCoef[j] * u0[j] - u1[j];
         u1[j] = t;
      }
   }
   for(j=0; j<NUMTONES; j++) {  /* feedforward */
      power[j] = ((u0[j] * u0[j] + u1[j] * u1[j] - m_pCoef[j] * u0[j] * u1[j]));
   }
   return(0);
}

/*
 * detect which signals are present.
 * return values defined in the include file
 * note: DTMF 3 and MF 7 conflict.  To resolve
 * this the program only reports MF 7 between
 * a KP and an ST, otherwise DTMF 3 is returned
 */
int CHuDtmfDetector::Detect(short *data, int nInLen)
{
   double power[NUMTONES],thresh,maxpower;
   int on[NUMTONES],on_count;
   int bcount, rcount, ccount;
   int row, col, b1, b2, i;
   int r[4],c[4],b[8];

// changed by wjstkddn92 20201016 vms dtmf detect issue (MFmode => m_nMFMode)
//   static int MFmode=0;

   maxpower = 0;
   CalcPower(data, power, nInLen);
   for(i=0; i<NUMTONES;i++) {
      if(power[i] > maxpower) {
         maxpower = power[i]; 
      }
   }
//fprintf(stderr,"KKK DTMF_SIL set %d maxpower %f %f\n",__LINE__,maxpower,m_dPowerThresh);
   if(maxpower < m_dPowerThresh) { /* silence? */ 
      return(DTMF_SIL);
   }
   thresh = PEAK_RANGE * maxpower;    /* allowable range of powers */
   for(i=0, on_count=0; i<NUMTONES; i++) {
      if(power[i] > thresh) { 
         on[i] = 1;
         on_count ++;
//fprintf(stderr,"KKKKKK oncount:%d power[%d]=%f thresh=%f\n",on_count,i,power[i],thresh);
      } else {
         on[i] = 0;
      }
   }

   if(on_count == 1) {
      if(on[B7]) {
         return(DTMF_24);
      }
      if(on[B8]) {
         return(DTMF_26);
      }
      return(-1);
   }

   if(on_count == 2) {
      if(on[X1] && on[X2]) {
         return(DTMF_DT);
      }
      if(on[X2] && on[X3]) {
         return(DTMF_RING);
      }
      if(on[X3] && on[X4]) {
         return(DTMF_BUSY);
      }

      b[0]= on[B1]; b[1]= on[B2]; b[2]= on[B3]; b[3]= on[B4];
      b[4]= on[B5]; b[5]= on[B6]; b[6]= on[B7]; b[7]= on[B8];
      c[0]= on[C1]; c[1]= on[C2]; c[2]= on[C3]; c[3]= on[C4];
      r[0]= on[R1]; r[1]= on[R2]; r[2]= on[R3]; r[3]= on[R4];

      for(i=0, bcount=0; i<8; i++) {
         if(b[i]) {
            bcount++;
            b2 = b1;
            b1 = i;
         }
      }
      for(i=0, rcount=0; i<4; i++) {
         if(r[i]) {
            rcount++;
            row = i;
         }
      }
      for(i=0, ccount=0; i<4; i++) {
         if(c[i]) {
            ccount++;
            col = i;
         }
      }

//fprintf(stderr,"KKKKKK oncount:%d rcount:%d ccount:%d row:%d col:%d\n",on_count,rcount,ccount,row,col);
      if(rcount==1 && ccount==1) {   /* DTMF */
         if(col == 3) { /* A,B,C,D */
            return(DTMF_A + row);
         } else {
            if(row == 3 && col == 0 ) {
               return(DTMF_STAR);
            } else
            if(row == 3 && col == 2 ) {
               return(DTMF_PND);
            } else
            if(row == 3) {
               return(DTMF_0);
            } else
            if(row == 0 && col == 2) {   /* DTMF 3 conflicts with MF 7 */
//               if(!MFmode) {
               // changed by wjstkddn92 20201016 vms dtmf detect issue (MFmode => m_nMFMode)
               if(!m_nMFmode) {
                 return(DTMF_3);
               }
            } else {
               return(DTMF_1 + col + row*3);
            }
         }
      }

      if(bcount == 2) { /* MF */
         /* b1 has upper number, b2 has lower */
         switch(b1) {
           case 7: return( (b2==6)? DTMF_2426: -1); 
           case 6: return(-1);
           case 5: if(b2==2 || b2==3) {  /* KP */
                      // changed by wjstkddn92 20201016 vms dtmf detect issue (MFmode => m_nMFMode)
                      //MFmode=1;
                      m_nMFmode=1;
                   } else
                   if(b2==4) { /* ST */
                      // changed by wjstkddn92 20201016 vms dtmf detect issue (MFmode => m_nMFMode)
                      //MFmode=0;
                      m_nMFmode=0;
                   } 
                   return(DTMF_C11 + b2);
           /* MF 7 conflicts with DTMF 3, but if we made it
            * here then DTMF 3 was already tested for 
            */
           case 4: return( (b2==3)? DTMF_0: DTMF_7 + b2);
           case 3: return(DTMF_4 + b2);
           case 2: return(DTMF_2 + b2);
           case 1: return(DTMF_1);
         }
      }
      return(-1);
   }

   if(on_count == 0) {
      return(DTMF_SIL);
   }
   return(-1); 
}

#define NONE_DTMF(x) \
   ((x) == DTMF_SIL || (x) == DTMF_24 || (x) == DTMF_26 || \
    (x) == DTMF_2426 || (x) == DTMF_DT || (x) == DTMF_BUSY || \
    (x) == DTMF_RING )


int CHuDtmfDetector::Decode(unsigned char *data, int nInLen, DTMF_CODE * pDtmfCode, int *pnDuration)
{
   if(!m_bOpened) return -1;

   DTMF_CODE nDtmf = DTMF_SIL;
   DTMF_CODE x;
   int nSample;
   unsigned char *pSample = data;
   int nInLeft = nInLen;

   while(nInLeft > 0) {
      if(m_nSampleLeft + nInLeft < m_nSampleWinSize) {
         memcpy(m_pSample + m_nSampleLeft, pSample, nInLeft);
         m_nSampleLeft += nInLeft;
         break;
      } else {
         nSample = m_nSampleWinSize - m_nSampleLeft;
         memcpy(m_pSample + m_nSampleLeft, pSample, nSample);
         nInLeft -= nSample;
         pSample += nSample;
         m_nSampleLeft = 0;
      }

      x = (DTMF_CODE)Detect((short*)m_pSample, m_nSampleWinSize / sizeof(short));
      if(x < 0) {
         x = DTMF_NONE;
         //x= m_nLastTone;
      } 

      if(m_bDetectOnce) {
         if( (x != m_nLastTone) &&
             (m_nDtmfDuration >= m_nMinDuration) ) {
            nDtmf = m_nLastTone;
            if(pnDuration != NULL) {
               *pnDuration = m_nDtmfDuration;
            }
            m_nDtmfDuration = 0;
         }
         m_nLastTone = x;
         m_nDtmfDuration += (int)(1000.0 * m_nSampleWinSize / sizeof(short) / m_nSampleRate);
         if(m_nLastTone == DTMF_SIL || m_nLastTone == DTMF_NONE)
            m_nDtmfDuration = 0;
#if 0//KKKKK redrock
   char time_str[30];time_str[0]='\0';
   getFormattedTime(time_str);
 fprintf(stderr,"KKK code:%d last:%ld mindur:%d dutaion:%d %s\n",x,m_nLastTone,m_nMinDuration,m_nDtmfDuration,time_str);
#endif
      } else {
         nDtmf = x;
      }
   }

   *pDtmfCode = nDtmf;
   return 0;
}



