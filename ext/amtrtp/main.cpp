#ifdef WIN32
#include<windows.h>
#endif
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

#if 1
#ifndef WIN32
#define Sleep(u) usleep((u)*1000)
#include <unistd.h>
#endif
#endif
#include "amtsock.h"
#include "amtrtcp.h"
#include "amtrtpcomm.h"



using namespace AMT;
#if 0
void TestRTPComm()
{
   int i, rc;
   int nPort = 20000;
   int nCount = 100;
   int *pnFd = new int[nCount];
   for(i=0; i<nCount; i++) {
      printf("# AMTSocket Test %d\n", i);
      rc = pnFd[i] = AMT::CAmtSock::Socket(AMT::CAmtSock::UDP);
      printf("# Socket %d\n", rc);
      rc = AMT::CAmtSock::Bind(pnFd[i], "127.0.0.1", nPort+i, 16384+(16384*i)); 
      rc = AMT::CAmtSock::Bind(pnFd[i], "127.0.0.1", nPort+i, 16384+(16384*i)); 
      printf("# Bind %d\n", rc);
   }   
   for(i=0; i<nCount; i++) {
      AMT::CAmtSock::Close(&pnFd[i]); 
   }   
}
#endif
int main(int argc, char* argv[]) 
{
   int rc;
   //TestRTPComm();
   
   AMT::CRtcpReport *pRtcp = new AMT::CRtcpReport;
   pRtcp->Open("TEST", 123456789, 90000);

   rc = pRtcp->PacketSent(1024, 100, 100000000);
   Sleep(100);
   rc = pRtcp->PacketSent(1024, 101, 100009000);
   Sleep(100);
   rc = pRtcp->PacketSent(1024, 102, 100018000);
   Sleep(100);
   rc = pRtcp->PacketSent(1024, 103, 100027000);
   Sleep(100);

   rc = pRtcp->PacketReceived(234567891, 1025, 200, 100000000);
   rc = pRtcp->PacketReceived(234567891, 1025, 201, 100009000);
   rc = pRtcp->PacketReceived(234567891, 1025, 203, 100027000);
   rc = pRtcp->PacketReceived(234567891, 1025, 204, 100036000);

   unsigned char *pPacket = new unsigned char[2048];
   int nLen;
   rc = pRtcp->MakeReport(pPacket, 2048, &nLen);

   {
      FILE* fp = fopen("out.dat", "wb");
      fwrite(pPacket, nLen, 1, fp);
      fclose(fp);
   }

   {
      FILE* fp = fopen("out.dat", "rb");
      nLen = fread(pPacket, 1, 2048, fp);
      fclose(fp);
      pRtcp->ParseReport(pPacket, nLen);
      AMT::CRtcpReport::Report report;
      for(int i=0;i<pRtcp->GetReportCount();i++) {
         pRtcp->GetReport(i, &report);
      }
   }

   return 0;
}
