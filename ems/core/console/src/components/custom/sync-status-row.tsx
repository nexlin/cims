import type { ReactNode } from 'react'
import { AlertTriangle, RefreshCw } from 'lucide-react'
import { Badge } from '@core/components/ui/badge'
import { Button } from '@core/components/ui/button'
import { Switch } from '@core/components/ui/switch'
import { StatusDot } from './status-dot'

/**
 * 그룹 › 패키지 설정 상단 상태 줄. 정본 = Figma `02 Components` SyncStatusRow (459:7511).
 * **드리프트 0건이면 경고를 아예 그리지 않는다** — 0건에 경고색을 쓰지 않는다(DESIGN-RULES §1-7).
 *
 * 시안 줄에는 배지만 있고 토글이 없는데, 자동 교정을 끄는 스위치는 살아 있어야 한다
 * (S3 안내문도 "동기화 스위치 포함" 이라고 가리킨다). 그래서 계약이 허용하는 조합으로
 * 둘을 나란히 둔다 — **Switch 가 조작, Badge 가 결과 표시**다. Switch 를 쓴 것은 이 토글이
 * 저장 버튼 뒤가 아니라 **즉시 적용**이기 때문이다 (DESIGN-RULES §2 Switch 조건).
 */
export function SyncStatusRow({ syncOn, onToggle, toggling, activeNode, drift, extra, onRefresh, refreshing }: {
  syncOn: boolean
  onToggle?: () => void
  toggling?: boolean
  /** ACTIVE 노드 이름. 판정 전이면 null — 그 사유를 대신 적는다. */
  activeNode: string | null
  /** 드리프트 문구. 0건이면 넘기지 않는다 (경고를 그리지 않기 위해). */
  drift?: ReactNode
  /** 버전 혼재처럼 이 줄에서 더 알릴 것 */
  extra?: ReactNode
  onRefresh?: () => void
  refreshing?: boolean
}) {
  return (
    <div className="flex flex-wrap items-center gap-3">
      {onToggle && (
        <Switch checked={syncOn} disabled={toggling} onCheckedChange={onToggle}
                aria-label="공통 설정 자동 동기화"
                title={syncOn
                  ? 'ON — ACTIVE 기준으로 STANDBY 공통 설정을 자동 교정 (이벤트+주기). 업데이트 작업 전 OFF 로 전환하세요.'
                  : 'OFF — 자동 교정 정지. 멤버별로 독립 편집 (업그레이드 창). 작업 완료 후 ON 으로.'} />
      )}
      <Badge variant={syncOn ? 'successSoft' : 'neutralSoft'}>
        동기화 {syncOn ? 'ON' : 'OFF'}
      </Badge>
      {activeNode
        ? <StatusDot tone="success" label={`ACTIVE ${activeNode}`} />
        : <span className="text-xs text-muted-foreground">ACTIVE 판정 불가 (heartbeat 관측 대기)</span>}
      {drift && (
        <>
          <span className="text-xs text-muted-foreground">·</span>
          <span className="inline-flex items-center gap-1 text-xs text-warning-on">
            <AlertTriangle size={13} /> {drift}
          </span>
        </>
      )}
      {extra}
      <div className="flex-1" />
      {onRefresh && (
        <Button variant="ghost" onClick={onRefresh} disabled={refreshing}>
          <RefreshCw /> 새로고침
        </Button>
      )}
    </div>
  )
}
