// 코어 컨트롤 위젯 — 이 페이지의 shape 위젯들이 함께 볼 **데이터 소스**를 고른다.
//
// 왜 위젯마다 고르지 않는가: 한 화면(예: 메시지 통계)의 차트와 표는 같은 대상을 봐야 한다.
// 위젯별 dropdown 이면 차트는 SIP, 표는 CMP 를 보는 상태가 만들어지고 대상 표기도 두 곳에 흩어진다.
// 그래서 대상 선택은 기간·단위와 같은 **페이지 조회 조건**으로 다룬다 (pageParams — `src`).
//
// 후보는 배치가 정한다(config.sources = 소스 id 목록). 지정하지 않으면 그 shape 계약을 만족하는
// 카탈로그 전체 — 화면 목적과 무관한 소스(VoLTE/PTT 서비스 KPI 등)까지 섞이므로 목적 화면에서는
// 반드시 열거한다. 값은 **id** 로 적는다 (표시 이름은 카탈로그가 소유 — identifier_model).
import type { WidgetDef, WidgetProps } from '../types'
import { usePageControl, usePageParam } from '../pageParams'
import { useDataSourceCatalog, sourcesForShape } from '../shapes/sourceRegistry'
import type { ShapeKind } from '../shapes/types'

// 'a, b , c' → ['a','b','c'] (빈 항목 제거). 편집기 [⚙] 에서 한 줄로 편집할 수 있게 문자열로 받는다.
function idList(v: unknown): string[] {
  return typeof v === 'string' ? v.split(',').map(s => s.trim()).filter(Boolean) : []
}

function SourcePickerWidget({ config }: WidgetProps) {
  usePageControl('source')
  const [src, setSrc] = usePageParam('src')
  const { sources: catalog, loading } = useDataSourceCatalog()
  const shape = (typeof config?.shape === 'string' ? config.shape : 'time-bar') as ShapeKind
  const want = idList(config?.sources)
  // 열거된 후보는 **적힌 순서대로** 보인다(배치가 정한 순서 = 화면 순서).
  const cands = want.length
    ? want.map(id => catalog.find(s => s.id === id)).filter((s): s is NonNullable<typeof s> => !!s)
    : sourcesForShape(shape, catalog)
  // 선택값이 후보 밖(첫 진입·후보 변경)이면 첫 후보를 활성으로 본다.
  const active = cands.some(s => s.id === src) ? src : (cands[0]?.id ?? '')
  return (
    <div className="tab-nav">
      {cands.map(s => (
        <button key={s.id} className={`tab-btn ${active === s.id ? 'tab-btn--active' : ''}`}
                onClick={() => setSrc(s.id)}>{s.label}</button>
      ))}
      {cands.length === 0 && (
        <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>
          {loading ? '소스 로딩…' : '(후보 소스 없음)'}
        </span>
      )}
    </div>
  )
}

export const sourcePickerWidget: WidgetDef = {
  id: 'core.source-picker',
  title: '대상 선택 (데이터 소스)',
  category: 'control',
  component: SourcePickerWidget,
  configFields: [
    { key: 'sources', label: '후보 소스 id (쉼표)', type: 'text',
      placeholder: 'cims.msg.sip, cims.msg.cmp' },
  ],
  defaultSize: { w: 12, h: 3 },
}
