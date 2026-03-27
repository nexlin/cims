#ifndef __HU_G711__H__
#define __HU_G711__H__


class CHuG711Codec
{
public:
   int DecodeALaw (unsigned char * pSrcFrame, unsigned char * pDstFrame, int nSampleCount);
   int DecodeMuLaw(unsigned char * pSrcFrame, unsigned char * pDstFrame, int nSampleCount);
   int EncodeALaw (unsigned char * pSrcFrame, unsigned char * pDstFrame, int nSampleCount);
   int EncodeMuLaw(unsigned char * pSrcFrame, unsigned char * pDstFrame, int nSampleCount);
};

#endif // __HU_G711__H__
