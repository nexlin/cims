#ifndef _MD5_H_
#define _MD5_H_

void SipMd5String( const char * pszPlainText, char result[33] );
void SipMd5Byte( const char * pszPlainText, unsigned char digest[16] );

#endif
