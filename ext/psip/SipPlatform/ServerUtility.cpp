#include "ServerUtility.h"
#include "Log.h"
#include "SipPlatformDefine.h"
#include <stdio.h>
#include <string.h>


#ifndef WIN32
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef __APPLE__
#include <sys/prctl.h>
#endif

#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>


#endif

#include "MemoryDebug.h"


void SetCoreDumpEnable() {
#ifndef WIN32
#ifndef __APPLE__
  prctl(PR_SET_DUMPABLE, 1);
#endif
#endif
}

/**
 * @ingroup SipPlatform
 * @brief 데몬 모드로 동작한다.
 * @param bIsFork 데몬 모드로 동작하면 true 를 넣어주고 그렇지 않으면 false 를
 * 넣어준다.
 * @returns true 를 리턴한다.
 */
bool Fork(bool bIsFork) {
#ifndef WIN32
  if (bIsFork) {
    if (fork() != 0)
      exit(0);

    // 1번째 자식 프로세스 시작 -> 세션 리더로 변경한다.
    setsid();
    signal(SIGHUP, SIG_IGN);
    if (fork() != 0)
      exit(1);

// 2번째 자식 프로세스 시작 -> 작업 디렉토리를 변경한다.
#if 0
	  if( chdir( "/tmp" ) == -1 )
		{
			CLog::Print( LOG_ERROR, "chdir(/tmp) error" );
		}
#endif
  }
#endif

  return true;
}

/**
 * @ingroup SipPlatform
 * @brief 프로그램 실행 사용자를 수정한다.
 * @param pszUserId 프로그램 실행 사용자
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool ChangeExecuteUser(const char *pszUserId) {
#ifndef WIN32
#ifndef ANDROID

  struct passwd sttPassWord, *psttPassWord = NULL;
  char szError[255], szPassWord[1024];

  if (getpwnam_r(pszUserId, &sttPassWord, szPassWord, sizeof(szPassWord),
                 &psttPassWord) != 0 ||
      psttPassWord == NULL) {
    CLog::Print(LOG_ERROR, "getpwnam(%s) error(%d) - %s", pszUserId, errno,
                strerror_r(errno, szError, sizeof(szError)));
    return false;
  }

  if (setuid(psttPassWord->pw_uid) != 0) {
    CLog::Print(LOG_ERROR, "seteuid(%s) error(%d) - %s", pszUserId, errno,
                strerror_r(errno, szError, sizeof(szError)));
    return false;
  }

  if (psttPassWord->pw_dir) {
    if (chdir(psttPassWord->pw_dir) == -1) {
      CLog::Print(LOG_ERROR, "chdir(%s) error", psttPassWord->pw_dir);
    }
#ifndef __APPLE__
    prctl(PR_SET_DUMPABLE, 1);
#endif
  }

#endif
#endif

  return true;
}

/**
 * @ingroup SipPlatform
 * @brief 쓰레드를 시작한다.
 * @param pszName					쓰레드 이름
 * @param lpStartAddress	쓰레드 함수
 * @param lpParameter			쓰레드 인자
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
#ifdef WIN32
bool StartThread(const char *pszName, LPTHREAD_START_ROUTINE lpStartAddress,
                 LPVOID lpParameter) {
  DWORD dwThreadId;
  HANDLE hThread;

  hThread = CreateThread(NULL, 0, lpStartAddress, lpParameter, 0, &dwThreadId);
  if (hThread == NULL) {
    return false;
  }

  CloseHandle(hThread);

  return true;
}
#else
bool StartThread(const char *pszName, void *(*lpStartAddress)(void *),
                 void *lpParameter) {
  pthread_t iThread;

  if (pthread_create(&iThread, NULL, lpStartAddress, lpParameter) != 0) {
    return false;
  }

  pthread_detach(iThread);

  return true;
}
#endif
