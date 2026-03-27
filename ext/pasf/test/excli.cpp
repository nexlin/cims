#include <iostream>
#include <unistd.h>
#include "psocket.h"

using namespace std;

int main ()
{
  int nRet = 0;
  int nCnt = 0;
  PNbSocket::Ptr spSocket(new PNbSocket());
  string str("127.0.0.1");

  bool test = spSocket->IsConnect();
  cout << test << endl;

  while (1) {
    if (!spSocket->IsConnect()) {
      spSocket->Connect(str, 14355);   
    }
    else {
      cout << "test2: " << spSocket->GetConnectAddr() << endl;
      while(1) {
#if 0
        string strMsg("Test 800000000");
        nRet = spSocket->Send(strMsg.c_str(), 7, MSG_NOSIGNAL);
#else
        char szTest[128];
        memset(szTest, 0x00, 64);
        nRet = spSocket->Recv(szTest, 64);
#endif

        if (nRet == SOCKET_ERROR) {

          if (errno == EAGAIN) {
            cout << "SEND EAGAIN" << endl;
          }
          else {
            cout << "SEND ERROR:" << errno << endl;
          }
        }
        else if (nRet == 0) {
            cout << "nRet==0, ERROR:" << errno << endl;
        }
        else {
          cout << "SEND OK: " << nRet << endl;
        }
        nCnt++;

        if (nCnt == 20) {
          cout << "End" << endl;
          return 0;
        }
        sleep(2);
      }
    }
    sleep(1);
  }

  cout << "test" <<endl;
}
