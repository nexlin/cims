// CSP flow 실패 등록 해제의 제휴 유예 회수 단위시험 — csp/AffiliationGrace.h `CAffiliationGrace`
//   (registration_binding_set.md §4.4). flow 실패로 풀린 등록은 제휴 회수를 그 등록의 수명 끝까지 미루고, 그 안의 재등록·해지가
//   유예를 거두며, 시한이 지난 것만 한 번 내준다는 판정을 엔진 없이 고정한다.
//   빌드·실행은 S1-UNIT-CSP(verify/lib/items/stage1/unit_csp.py)가 수행한다.
#include "AffiliationGrace.h"
#include <cstdio>
static int fail=0;
#define CK(n,c) do{ if(!(c)){printf("FAIL %s\n",n);fail++;}else printf("ok   %s\n",n);}while(0)
int main(){
  CAffiliationGrace g;
  g.Defer("+82500000001",1000);
  CK("pending",g.Pending("+82500000001") && g.Size()==1);
  CK("not due before deadline",g.TakeDue(999).empty() && g.Pending("+82500000001"));
  // 재등록 — 유예를 거둔다(제휴 유지), 다시 거두면 없다
  CK("cancel on re-register",g.Cancel("+82500000001") && !g.Pending("+82500000001"));
  CK("cancel twice",!g.Cancel("+82500000001"));
  // 시한이 지나면 한 번만 내준다
  g.Defer("+82500000002",2000);
  auto d=g.TakeDue(2000);
  CK("due at deadline",d.size()==1 && d[0]=="+82500000002");
  CK("taken once",g.TakeDue(5000).empty() && g.Size()==0);
  // 다시 미루면 늦은 시한을 남긴다(앞서 미룬 것보다 이른 시한으로 당기지 않는다)
  g.Defer("+82500000003",3000);
  g.Defer("+82500000003",2500);
  CK("keeps later deadline",g.TakeDue(2999).empty());
  g.Defer("+82500000003",4000);
  CK("extends deadline",g.TakeDue(3999).empty() && g.TakeDue(4000).size()==1);
  // 여럿 — 시한이 지난 것만
  g.Defer("a",10); g.Defer("b",20); g.Defer("c",30);
  auto e=g.TakeDue(20);
  CK("only due ones",e.size()==2 && g.Pending("c") && !g.Pending("a") && !g.Pending("b"));
  printf(fail?"FAILED %d\n":"ALL PASS\n",fail);
  return fail?1:0;
}
