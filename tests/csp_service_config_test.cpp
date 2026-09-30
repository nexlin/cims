// CSP service-config 해석 단위시험 — csp/CspServiceConfig.h `DurationMs` / `Parse`.
//   TS 24.484 §8.4 문서의 on-network <transmit-time><time-limit>(T2)·<fc-timers-counters> 를 CMP floor_timers 값(초·횟수)으로
//   옮긴다. xs:duration(PT<h>H<m>M<n>S, 소수 초), on-network 범위 한정(off-network 의 transmit-time 무시), 이름 경계
//   (<group-time-limit> 은 <time-limit> 이 아니다), 문서에 없는 값 = -1 이 요점이다.
//   빌드·실행은 S1-UNIT-CSP(verify/lib/items/stage1/unit_csp.py)가 수행한다.
#include "CspServiceConfig.h"
#include <cstdio>
static int fail=0;
#define CK(n,c) do{ if(!(c)){printf("FAIL %s\n",n);fail++;}else printf("ok   %s\n",n);}while(0)
int main(){
  CK("dur s",CCspServiceConfig::DurationMs("PT4S")==4000);
  CK("dur frac",CCspServiceConfig::DurationMs("PT0.5S")==500);
  CK("dur hms",CCspServiceConfig::DurationMs("PT1H2M3S")==3723000);
  CK("dur bad",CCspServiceConfig::DurationMs("4S")==-1 && CCspServiceConfig::DurationMs("PT4")==-1 && CCspServiceConfig::DurationMs("PTS")==-1);
  const char* doc =
    "<service-configuration-info xmlns=\"urn:3gpp:ns:mcpttServiceConfig:1.0\"><service-configuration-params domain=\"d\">"
    "<on-network><emergency-call><group-time-limit>PT99S</group-time-limit></emergency-call>"
    "<transmit-time><time-limit>PT45S</time-limit></transmit-time>"
    "<fc-timers-counters><T1-end-of-rtp-media>PT4S</T1-end-of-rtp-media><T3-stop-talking-grace>PT3S</T3-stop-talking-grace>"
    "<T7-floor-idle>PT0S</T7-floor-idle><T8-floor-revoke>PT1S</T8-floor-revoke><T20-floor-granted>PT1.5S</T20-floor-granted>"
    "<C7-floor-idle>4</C7-floor-idle><C20-floor-granted>5</C20-floor-granted></fc-timers-counters></on-network>"
    "<off-network><transmit-time><time-limit>PT7S</time-limit></transmit-time></off-network>"
    "</service-configuration-params></service-configuration-info>";
  CspFloorParams f;
  CK("parse",CCspServiceConfig::Parse(doc,f) && f.bValid);
  CK("T2 on-network time-limit",f.iT2Sec==45);
  CK("T1 T3",f.iT1Sec==4 && f.iT3Sec==3);
  CK("T7 zero",f.iT7Sec==0);
  CK("T8 T20 truncate",f.iT8Sec==1 && f.iT20Sec==1);
  CK("C7 C20",f.iC7==4 && f.iC20==5);
  CspFloorParams g;
  CK("absent = -1",CCspServiceConfig::Parse("<service-configuration-info/>",g) && g.iT1Sec==-1 && g.iT2Sec==-1 && g.iC7==-1);
  CspFloorParams h;
  CK("old root rejected",!CCspServiceConfig::Parse("<mcptt-service-config/>",h));
  printf("%s (%d fail)\n",fail?"FAIL":"PASS",fail); return fail?1:0;
}
