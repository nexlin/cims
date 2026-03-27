#ifndef     __HU_DTMF_H__
#define     __HU_DTMF_H__


typedef struct {
   unsigned char ucDetectDtmf;
   unsigned char ucReserved[7];
   int nCode;
   int nDuration;
} DTMFINFO, *PDTMFINFO;

class CHuDtmf 
{
public:
   typedef enum {
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
   } DTMF_CODE;

   CHuDtmf(){};
   ~CHuDtmf(){};

protected:
   int  m_nSampleRate;  // 8000
   int  m_nDtmfDuration;
   int  m_nSampleLeft;
   DTMF_CODE  m_nLastTone;
};

class CHuDtmfDetector : public CHuDtmf
{
public:
   // nTimeWinSize : 
   //    minimal unit of sample(in millisecond) into which the detector seach DTMF. 
   //    irrespective of input-sample size. RECOMMENDATION : 30ms
   // nMinDuration : 
   //    least time(in millisecond) of continuous DTMF tone, 
   //    if so, that is considered as a valid DTMF. RECOMMENDATION : 100ms
   CHuDtmfDetector();
   ~CHuDtmfDetector();

   // bDetectOnce : 
   //    TRUE  = considers continual same DTMF as one DTMF
   //    FALSE = print every DTMF in sample of nTimeWinSize.
   int Open(int nSampleRate, int nTimeWinSize=30, int nMinDuration=100, int nPowerThresh=10, bool bDetectOnce=false);
   int Close();
   int Decode(unsigned char *data, int nInLen, DTMF_CODE * pDtmfCode, int *pnDuration);

protected:
   int CalcPower(short *data, double *power, int nInLen);
   int Detect(short *data, int nInLen);
   int CalcCoef();

protected:
   bool           m_bOpened;
   int            m_nTimeWinSize; // millisecond.
   int            m_nMinDuration; // millisecond.
   int            m_nSampleWinSize;
   double         m_dPowerThresh; // ??? 
   bool           m_bDetectOnce;
   float *        m_pCoef;
   unsigned char *m_pSample;

   // added by wjstkddn92 20201016 vms dtmf detect issue
   int            m_nMFmode;
};


class CHuDtmfGenerator : public CHuDtmf
{
public:
   typedef struct {
      double f1; // frequency1, 0: off/ignore, -1:End of array.
      double f2; // frequency2, 0: off/ignore
      double f3; // frequency3, 0: off/ignore
      double f4; // frequency4, 0: off/ignore
      int    tm; // duration in millisecond. -1:continuous.
   } ATone;

public:
   CHuDtmfGenerator();
   ~CHuDtmfGenerator();

   // Usage 1: Open -> { Set -> { Encode } } -> Close
   // Usage 2: { MakeTone }
   int  Open(int nSampleRate, int nDelay=0); //nDelay : msec
   int  Set(DTMF_CODE nDtmfCode, int nDuration, int nPower);
   // Return: Left samples in byte.
   int  Encode(unsigned char * pFrame, int nFrameSize, bool bMix=false); // bMix : mix DTMF with existing audio.
   int  CalcFrameLen(int nSampleRate, int nDuration);
   int  MakeTone(unsigned char * pFrame, int nFrameLen, int nSampleRate, DTMF_CODE nDtmfCode, int nPower);
   bool IsSet() { return m_bSet; }
   int  Close();

protected:
   void Done();
   bool m_bIsDtmf;
   bool m_bOpened;
   bool m_bSet;
   int  m_nDelaySamples;
   int  m_nDelaySampleLeft;
   double  m_dMaxPower;    
   unsigned int m_uRadOfLowFreq;
   unsigned int m_uRadOfHighFreq;
   unsigned int m_uRadPerSampleOfLowFreq;
   unsigned int m_uRadPerSampleOfHighFreq;
   int  m_nEncSamples;
   ATone *m_pToneList;

protected:
   int  EncodeDtmf(unsigned char * pFrame, int nFrameSize, bool bMix=false);
   int  EncodeTone(unsigned char * pFrame, int nFrameSize, bool bMix=false);

};


#endif//     __HU_DTMF_H__
