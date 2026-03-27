#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hudtmf.h"
#include "hug711.h"

#if 0
int detect_test(int argc, char *argv[])
{
   CHuDtmfDetector *pDetect = new CHuDtmfDetector();
   CHuG711Codec * pG711 = new CHuG711Codec;
   unsigned char* pPCMFrame;
   unsigned char* pG711Frame;
   CHuDtmf::DTMF_CODE nDtmf;
   int nSampleRate, nSamplePerFrame, nRead, n;
   unsigned char *pData, *pPos;


   //sprintf(szDstFile, "%s_out.alaw", argv[1]);
   //sprintf(szDstFile2, "%s_out.alaw.pcm", argv[1]);
   FILE* fp = fopen(argv[1], "rb");
   //FILE* fp2 = fopen(szDstFile2, "wb+");
   fseek(fp, 0, SEEK_END);
   int nDataLen = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   pData = new unsigned char[nDataLen];
   int nRemains = nDataLen;
   pPos = pData;
   while (nRemains > 0) {
      nRead = fread(pPos, 1, nRemains, fp);
      if(nRead < 1) break;
      nRemains -= nRead;
      pPos += nRead;
   }
   fclose(fp);

   nSampleRate = 8000;
   nSamplePerFrame = 80;

   pPCMFrame = new unsigned char[nSamplePerFrame*2];
   pG711Frame = new unsigned char[nSamplePerFrame];

   int nLoop = 1;
   for(int nIndex = 0; nIndex < nLoop; nIndex++) {
      pDetect->Open(nSampleRate, 30, 500, true);
      nRead = 0;
      int nDuration;
      while(true) {
         if(nRead >= nDataLen) break;
         pG711->DecodeALaw(pData + nRead, pPCMFrame, nSamplePerFrame);
         //fwrite(pPCMFrame, 1, n*2, fp2);
      
         nDtmf = pDetect->Decode(pPCMFrame, nSamplePerFrame*2, &nDuration);

         if(nDtmf != CHuDtmf::DTMF_SIL && nDtmf != CHuDtmf::DTMF_NONE) 
         {
            //printf("## DTMF : %c(%d), Duration(%d), len(%6d)\n",'0' + nDtmf, nDtmf, nDuration, nRead);
         }
         nRead += nSamplePerFrame;
      }
      pDetect->Close();
   }

   delete pG711;
   delete pDetect;
   delete [] pPCMFrame;
   delete [] pG711Frame;
   delete [] pData;
   
   return 0;
}


int gen_test(int argc, char *argv[])
{
   unsigned char * pPCMFrame;
   unsigned char * pG711Frame;
   CHuDtmfGenerator *pGen = new CHuDtmfGenerator;
   CHuG711Codec * pG711 = new CHuG711Codec;
   char szDstFile[256];

   sprintf(szDstFile, "%s_out.alaw", argv[1]);
   FILE* fpIn = fopen(argv[1], "rb");
   FILE* fpOut = fopen(szDstFile, "wb+");

   int nDuration = 1000;

   int nPower = 100;
   int nSampleRate = 8000;
   int nSamplePerFrame = 80;

   pGen->Open(nSampleRate, nPower);
   pPCMFrame = new unsigned char[nSamplePerFrame*2];
   pG711Frame = new unsigned char[nSamplePerFrame];

   int nCount = 0;
   int n;
   while(true)
   {
      nCount++;
      n = fread(pPCMFrame, 1, nSamplePerFrame*2, fpIn); 
   
      if(n <= 0) break;

      if((nCount % 200) == 0) {
         static int nCode = 0;
         //pGen->Generate(pFrame, n, (CHuDtmf::DTMF_0), nDuration, true);
         pGen->Set((CHuDtmf::DTMF_CODE)(nCode++%28), nDuration);
         pGen->Encode(pPCMFrame, n, true, (CHuDtmf::DTMF_CODE)(nCode++%28), nDuration);
      } else {
         pGen->Encode(pPCMFrame, n, false, (CHuDtmf::DTMF_NONE), nDuration);
      }

      pG711->EncodeALaw(pPCMFrame, pG711Frame, n/2);
      fwrite(pG711Frame, 1, n/2, fpOut);

   }
   delete pGen;
   delete pG711;
   delete [] pPCMFrame;
   delete [] pG711Frame;

   fclose(fpIn);
   fclose(fpOut);
   return 0;
}

/*
  int sfd;
  char number[100];
  sfd = open(argv[0],O_RDWR);
  if(sfd<0) {
    perror(SOUND_DEV);
    return(-1);
  }
  printf("Enter fone number: ");
  gets(number);
  dial(sfd,number);
*/
#endif

void MakeDtmfToneTest(int argc, char ** argv)
{
   int             nPCMLen;
   unsigned char * pPCM;
   
   int rc;

   CHuDtmf::DTMF_CODE nDtmfCode = CHuDtmf::TONE_CONGESTION  ;

   int nSampleRate = 8000;
   int nDuration   = 10000;
   int nPower      = 100;
   if(argc > 3)   nDtmfCode   = (CHuDtmf::DTMF_CODE)atoi(argv[2]);
   if(argc > 4)   nSampleRate = atoi(argv[3]);
   if(argc > 5)   nDuration   = atoi(argv[4]);
   if(argc > 6)   nPower      = atoi(argv[5]);

   CHuDtmfGenerator *pGen = new CHuDtmfGenerator;

   nPCMLen = pGen->CalcFrameLen(nSampleRate, nDuration);
   pPCM = new unsigned char[nPCMLen];
   rc = pGen->MakeTone(pPCM, nPCMLen, nSampleRate, nDtmfCode, nPower);
   FILE * fp = fopen(argv[1], "wb+");
   fwrite(pPCM, 1, nPCMLen, fp);
   fclose(fp);
   delete []pPCM;
   delete pGen;
}

void MakeAllTone(int nDuration, int nCount, int nSleepTime)
{
static char wavheader[] = {
   0x52, 0x49, 0x46, 0x46, 0x24, 0xA8, 0x0E, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6D, 0x74, 0x20,
   0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x40, 0x1F, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00,
   0x02, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61, 0x00, 0xA8, 0x0E, 0x00 };


   int rc;

   struct {
      CHuDtmf::DTMF_CODE nCode;
      const char* szWavName;
      const char * szG711Name;
   } ToneList[] = {
      {CHuDtmf::DTMF_0            , "tone/DTMF_0.wav"           , "tone/DTMF_0.g711a"           },
      {CHuDtmf::DTMF_1            , "tone/DTMF_1.wav"           , "tone/DTMF_1.g711a"           },
      {CHuDtmf::DTMF_2            , "tone/DTMF_2.wav"           , "tone/DTMF_2.g711a"           },
      {CHuDtmf::DTMF_3            , "tone/DTMF_3.wav"           , "tone/DTMF_3.g711a"           },
      {CHuDtmf::DTMF_4            , "tone/DTMF_4.wav"           , "tone/DTMF_4.g711a"           },
      {CHuDtmf::DTMF_5            , "tone/DTMF_5.wav"           , "tone/DTMF_5.g711a"           },
      {CHuDtmf::DTMF_6            , "tone/DTMF_6.wav"           , "tone/DTMF_6.g711a"           },
      {CHuDtmf::DTMF_7            , "tone/DTMF_7.wav"           , "tone/DTMF_7.g711a"           },
      {CHuDtmf::DTMF_8            , "tone/DTMF_8.wav"           , "tone/DTMF_8.g711a"           },
      {CHuDtmf::DTMF_9            , "tone/DTMF_9.wav"           , "tone/DTMF_9.g711a"           },
      {CHuDtmf::DTMF_STAR         , "tone/DTMF_STAR.wav"        , "tone/DTMF_STAR.g711a"        },
      {CHuDtmf::DTMF_PND          , "tone/DTMF_PND.wav"         , "tone/DTMF_PND.g711a"         },
      {CHuDtmf::DTMF_A            , "tone/DTMF_A.wav"           , "tone/DTMF_A.g711a"           },
      {CHuDtmf::DTMF_B            , "tone/DTMF_B.wav"           , "tone/DTMF_B.g711a"           },
      {CHuDtmf::DTMF_C            , "tone/DTMF_C.wav"           , "tone/DTMF_C.g711a"           },
      {CHuDtmf::DTMF_D            , "tone/DTMF_D.wav"           , "tone/DTMF_D.g711a"           },
      {CHuDtmf::DTMF_C11          , "tone/DTMF_C11.wav"         , "tone/DTMF_C11.g711a"         },
      {CHuDtmf::DTMF_C12          , "tone/DTMF_C12.wav"         , "tone/DTMF_C12.g711a"         },
      {CHuDtmf::DTMF_KP1          , "tone/DTMF_KP1.wav"         , "tone/DTMF_KP1.g711a"         },
      {CHuDtmf::DTMF_KP2          , "tone/DTMF_KP2.wav"         , "tone/DTMF_KP2.g711a"         },
      {CHuDtmf::DTMF_ST           , "tone/DTMF_ST.wav"          , "tone/DTMF_ST.g711a"          },
      {CHuDtmf::DTMF_24           , "tone/DTMF_24.wav"          , "tone/DTMF_24.g711a"          },
      {CHuDtmf::DTMF_26           , "tone/DTMF_26.wav"          , "tone/DTMF_26.g711a"          },
      {CHuDtmf::DTMF_2426         , "tone/DTMF_2426.wav"        , "tone/DTMF_2426.g711a"        },
      {CHuDtmf::DTMF_DT           , "tone/DTMF_DT.wav"          , "tone/DTMF_DT.g711a"          },
      {CHuDtmf::DTMF_RING         , "tone/DTMF_RING.wav"        , "tone/DTMF_RING.g711a"        },
      {CHuDtmf::DTMF_BUSY         , "tone/DTMF_BUSY.wav"        , "tone/DTMF_BUSY.g711a"        },
      {CHuDtmf::DTMF_SIL          , "tone/DTMF_SIL.wav"         , "tone/DTMF_SIL.g711a"         },
      {CHuDtmf::TONE_DIAL         , "tone/TONE_DIAL.wav"        , "tone/TONE_DIAL.g711a"        },
      {CHuDtmf::TONE_RINGBACK     , "tone/TONE_RINGBACK.wav"    , "tone/TONE_RINGBACK.g711a"    },
      {CHuDtmf::TONE_BUSY         , "tone/TONE_BUSY.wav"        , "tone/TONE_BUSY.g711a"        },
      {CHuDtmf::TONE_CONGESTION   , "tone/TONE_CONGESTION.wav"  , "tone/TONE_CONGESTION.g711a"  },
      {CHuDtmf::TONE_WAITING      , "tone/TONE_WAITING.wav"     , "tone/TONE_WAITING.g711a"     },
      {CHuDtmf::TONE_HOLDING      , "tone/TONE_HOLDING.wav"     , "tone/TONE_HOLDING.g711a"     },
      {CHuDtmf::TONE_INTERCEPT    , "tone/TONE_INTERCEPT.wav"   , "tone/TONE_INTERCEPT.g711a"   },
      {CHuDtmf::TONE_SPECIALDIAL  , "tone/TONE_SPECIALDIAL.wav" , "tone/TONE_SPECIALDIAL.g711a" },
      {CHuDtmf::TONE_CONFIRM      , "tone/TONE_CONFIRM.wav"     , "tone/TONE_CONFIRM.g711a"     },
      {CHuDtmf::TONE_HOLWER       , "tone/TONE_HOLWER.wav"      , "tone/TONE_HOLWER.g711a"      },
      {CHuDtmf::TONE_SIT_RO1      , "tone/TONE_SIT_RO1.wav"     , "tone/TONE_SIT_RO1.g711a"     },
      {CHuDtmf::TONE_SIT_VC       , "tone/TONE_SIT_VC.wav"      , "tone/TONE_SIT_VC.g711a"      },
      {CHuDtmf::TONE_SIT_NC1      , "tone/TONE_SIT_NC1.wav"     , "tone/TONE_SIT_NC1.g711a"     },
      {CHuDtmf::TONE_SIT_IC       , "tone/TONE_SIT_IC.wav"      , "tone/TONE_SIT_IC.g711a"      },
      {CHuDtmf::TONE_SIT_RO2      , "tone/TONE_SIT_RO2.wav"     , "tone/TONE_SIT_RO2.g711a"     },
      {CHuDtmf::TONE_SIT_NC2      , "tone/TONE_SIT_NC2.wav"     , "tone/TONE_SIT_NC2.g711a"     },
      {CHuDtmf::TONE_SIT_IO       , "tone/TONE_SIT_IO.wav"      , "tone/TONE_SIT_IO.g711a"      },
      {CHuDtmf::TONE_END          , ""                     , ""                     },
   };
   int             nPCMLen = (160 * 10);
   unsigned char * pPCM, * pG711A;
   FILE * fpWav, * fpG711a;

   unsigned char bufMute[160];
   memset(bufMute, 0, sizeof(bufMute));

   CHuDtmfGenerator *pGen = new CHuDtmfGenerator;
   CHuG711Codec * pG711Codec = new CHuG711Codec;
   rc = pGen->Open(8000, 0);
   pPCM = new unsigned char[nPCMLen];
   pG711A = new unsigned char[nPCMLen/2];
   for(int i=0;;i++) {
      if(ToneList[i].nCode == CHuDtmf::TONE_END) break;
      fpWav = fopen(ToneList[i].szWavName, "wb+");
      fpG711a = fopen(ToneList[i].szG711Name, "wb+");
      fwrite(wavheader, 1, sizeof(wavheader), fpWav);
      for(int nCnt=0; nCnt<nCount; nCnt++) {
         while(true){
            rc = pGen->Set(ToneList[i].nCode, nDuration, 80);
            rc = pGen->Encode(pPCM, nPCMLen, false);
            fwrite(pPCM, 1, nPCMLen, fpWav);
            pG711Codec->EncodeALaw(pPCM, pG711A, nPCMLen/2);
            fwrite(pG711A, 1, nPCMLen/2, fpG711a);
            if(rc == 0) {
               break;
            }
         }
         if(nCount>1) {
            for(int k=0; k<nSleepTime/10; k++) {
               fwrite(bufMute, 1, 160, fpWav);
               pG711Codec->EncodeALaw(bufMute, pG711A, 80);
               fwrite(pG711A, 1, 80, fpG711a);
            }
         }
      }
      fclose(fpWav);
      fclose(fpG711a);
   }

   delete []pPCM;
   delete []pG711A;
   delete pGen;
   delete pG711Codec;
}


int main(int argc, char *argv[])
{
//   int i;
//   for(i=0;i<100;i++) {
//      gen_test(argc, argv);
//   }
   //detect_test(argc, argv);
   //MakeDtmfToneTest(argc, argv);

   int dur = atoi(argv[1]);
   MakeAllTone(dur, 1, 0);

   return 0;   
}

