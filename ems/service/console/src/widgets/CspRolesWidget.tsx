// CIMS 위젯 — CSP 모듈 역할 + 녹취 + 타이머 설정.
import { useSharedHealth } from '@core/widgets/useSharedHealth'
import type { WidgetDef } from '@core/widgets/types'
import { Badge } from '@core/components/ui/badge'

function CspRolesWidget() {
  const { data } = useSharedHealth()
  // 로딩 중에도 카드는 유지 — null 을 돌려주면 위젯이 통째로 사라졌다 팝인한다.
  if (!data) {
    return (
      <div className="panel p-4">
        <div className="font-semibold mb-2 text-base">CSP 모듈 역할</div>
        <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">불러오는 중…</div>
      </div>
    )
  }
  return (
    <div className="panel p-4">
      <div className="font-semibold mb-2 text-base">CSP 모듈 역할</div>
      <div className="flex gap-4 flex-wrap">
        {Object.entries(data.csp.roles).map(([k, v]) => (
          <Badge variant={v ? 'successSoft' : 'neutralSoft'} key={k} >
            {k}: {v ? 'ON' : 'OFF'}
          </Badge>
        ))}
        <Badge className="ml-auto" variant={data.record_enable ? 'brandSoft' : 'neutralSoft'}>
          녹취: {data.record_enable ? 'ON' : 'OFF'}
        </Badge>
      </div>
      {data.csp.timeouts && (
        <div className="flex gap-4 mt-2 text-sm text-muted-foreground">
          <span>등록 만료: {data.csp.timeouts.user_timeout}초</span>
          <span>Stale Call: {data.csp.timeouts.stale_call_timeout}초</span>
          <span>OPTIONS 주기: {data.csp.timeouts.send_options_period || '비활성'}초</span>
          {data.cmp.session_timeout != null && <span>CMP 세션: {data.cmp.session_timeout}초</span>}
        </div>
      )}
    </div>
  )
}

export const cspRolesWidget: WidgetDef = {
  id: 'cims.csp-roles',
  apis: ['stats.health'],
  title: 'CSP 모듈 역할',
  category: 'service',
  component: CspRolesWidget,
  defaultSize: { w: 12 },
}
