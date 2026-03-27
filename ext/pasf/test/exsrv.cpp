#include <iostream>
#include <unistd.h>
#include <sstream>
#include "psocket.h"

using namespace std;

int main ()
{
    char szTest[128];
    int nCnt = 0, nRet = 0;
    PNbSrvSocket::Ptr spServer(new PNbSrvSocket());
    PNbSocket::Ptr spSocket;
    string str;
    spServer->CreateServer(str,14355, 5);   

    while (1) {
      spSocket = spServer->Accept();
      if (spSocket) {
          cout << "Conn Success" << endl;

          while(1) {
#if 0
            memset(szTest, 0x00, 64);
            nRet = spSocket->Recv(szTest, 64);
#else
            stringstream ss;
            ss << "tbrm-" << nCnt;
            int nSize = ss.str().length();
            nRet = spSocket->Send((char*)&nSize, sizeof(int), MSG_NOSIGNAL);
#endif
            if (nRet == SOCKET_ERROR) {

              if (errno == EAGAIN) {
                  //cout << "RECV EAGAIN" << endl;
                  cout << "SEND EAGAIN" << endl;
              }
              else {
                  //cout << "RECV ERROR:" << errno << endl;
                  cout << "SEND ERROR:" << errno << endl;
              }
            }
            else {
              //cout << "RECV OK: " << szTest << ", " << nRet << endl;
              cout << "SEND OK Len: " << nSize << endl;
              nRet = spSocket->Send(ss.str().c_str(), nSize, MSG_NOSIGNAL);
              nCnt++;
              cout << "SEND OK Data: " << ss.str() << endl;
            }

            if (nCnt == 10000) {
                return 0;
            }
            sleep(2);
          }
      }
      sleep(1);
    }

    cout << "test" <<endl;
    return 0;
}
