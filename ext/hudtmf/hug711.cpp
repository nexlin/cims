#include <stdio.h>

#include "hug711.h"

#define         HUPCM_SIGN_BIT        (0x80)      /* Sign bit for a A-law byte. */
#define         HUPCM_QUANT_MASK      (0xf)       /* Quantization field mask. */
//#define         HUPCM_NSEGS           (8)         /* Number of A-law segments. */
#define         HUPCM_SEG_SHIFT       (4)         /* Left shift for segment number. */
#define         HUPCM_SEG_MASK        (0x70)      /* Segment field mask. */

#define         HUPCM_BIAS            (0x84)      /* Bias for linear code. */



enum {
   MAX_PCM_ENCODE_INDEX   = 256*256,
   MAX_PCM_DECODE_INDEX   = 256
};

enum PCM_TYPE{
   PCM_ALAW  = 0,
   PCM_MULAW = 1,
   PCM_MAX
};

static bool CreatePCMConvertTable();

static bool    s_bTableCreated = CreatePCMConvertTable();
static unsigned char * s_pEncodeTable[PCM_MAX] = { NULL, NULL } ;
static unsigned short * s_pDecodeTable[PCM_MAX] = { NULL, NULL };



static int alaw2linear(unsigned char a_val)
{
        int t;
        int seg;

        a_val ^= 0x55;

        t = a_val & HUPCM_QUANT_MASK;
        seg = ((unsigned)a_val & HUPCM_SEG_MASK) >> HUPCM_SEG_SHIFT;
        if(seg) t= (t + t + 1 + 32) << (seg + 2);
        else    t= (t + t + 1     ) << 3;

        return ((a_val & HUPCM_SIGN_BIT) ? t : -t);
}

static int ulaw2linear(unsigned char u_val)
{
        int t;

        /* Complement to obtain normal u-law value. */
        u_val = ~u_val;

        /*
         * Extract and bias the quantization bits. Then
         * shift up by the segment number and subtract out the bias.
         */
        t = ((u_val & HUPCM_QUANT_MASK) << 3) + HUPCM_BIAS;
        t <<= ((unsigned)u_val & HUPCM_SEG_MASK) >> HUPCM_SEG_SHIFT;

        return ((u_val & HUPCM_SIGN_BIT) ? (HUPCM_BIAS - t) : (t - HUPCM_BIAS));
}


static void build_xlaw_encoder_table(unsigned char *linear_to_xlaw, 
                             int (*xlaw2linear)(unsigned char),
                             int mask)
{
    int i, j, v, v1, v2;

    j = 0;
    for(i=0;i<128;i++) {
        if (i != 127) {
            v1 = xlaw2linear(i ^ mask);
            v2 = xlaw2linear((i + 1) ^ mask);
            v = (v1 + v2 + 4) >> 3;
        } else {
            v = 8192;
        }
        for(;j<v;j++) {
            linear_to_xlaw[8192 + j] = (i ^ mask);
            if (j > 0)
                linear_to_xlaw[8192 - j] = (i ^ (mask ^ 0x80));
        }
    }
    linear_to_xlaw[0] = linear_to_xlaw[1];
}

static void build_xlaw_decoder_table(unsigned short *xlaw_to_linear, 
                                    int (*xlaw2linear)(unsigned char))
{
   for(int i=0;i<MAX_PCM_DECODE_INDEX;i++)
      xlaw_to_linear[i] = alaw2linear(i);
}


static bool CreatePCMConvertTable()
{
   /* alaw to linear */
   s_pEncodeTable[PCM_ALAW] = new unsigned char[MAX_PCM_ENCODE_INDEX];
   build_xlaw_encoder_table(s_pEncodeTable[PCM_ALAW], alaw2linear, 0xd5);
   
   /* linear to alaw */
   s_pDecodeTable[PCM_ALAW] = new unsigned short[MAX_PCM_DECODE_INDEX];
   build_xlaw_decoder_table(s_pDecodeTable[PCM_ALAW], alaw2linear);


   /* mulaw to linear */
   s_pEncodeTable[PCM_MULAW] = new unsigned char[MAX_PCM_ENCODE_INDEX];
   build_xlaw_encoder_table(s_pEncodeTable[PCM_MULAW], ulaw2linear, 0xff);
   /* linear to mulaw */
   s_pDecodeTable[PCM_MULAW] = new unsigned short[MAX_PCM_DECODE_INDEX];
   build_xlaw_decoder_table(s_pDecodeTable[PCM_MULAW], ulaw2linear);
   return true;
}

int CHuG711Codec::DecodeALaw (unsigned char * pSrcFrame, 
                                unsigned char * pDstFrame, 
                                int nSampleCount)
{
   unsigned short * pPCMFrame = (unsigned short *) pDstFrame;
   unsigned short * pTable = s_pDecodeTable[PCM_ALAW];

   for(int i=0; i<nSampleCount; i++){
      pPCMFrame[i] = pTable[pSrcFrame[i]];
   }
   return 0;
}

int CHuG711Codec::DecodeMuLaw(unsigned char * pSrcFrame, 
                                unsigned char * pDstFrame, 
                                int nSampleCount)
{
   unsigned short * pPCMFrame = (unsigned short *) pDstFrame;
   unsigned short * pTable = s_pDecodeTable[PCM_MULAW];
   for(int i=0; i<nSampleCount; i++){
      pPCMFrame[i] = pTable[pSrcFrame[i]];
   }
   return 0;
}

int CHuG711Codec::EncodeALaw (unsigned char * pSrcFrame, 
                                unsigned char * pDstFrame, 
                                int nSampleCount)
{
   short * pPCMFrame = (short *) pSrcFrame;
   unsigned char  * pTable = s_pEncodeTable[PCM_ALAW];
   for(int i=0; i<nSampleCount; i++){
      pDstFrame[i] = pTable[(pPCMFrame[i] + 32768) >> 2];
   }
   return 0;
}

int CHuG711Codec::EncodeMuLaw(unsigned char * pSrcFrame, 
                                unsigned char * pDstFrame, 
                                int nSampleCount)
{
   short * pPCMFrame = (short *) pSrcFrame;
   unsigned char  * pTable = s_pEncodeTable[PCM_MULAW];
   for(int i=0; i<nSampleCount; i++){
      pDstFrame[i] = pTable[(pPCMFrame[i] + 32768) >> 2];
   }
   return 0;
}

