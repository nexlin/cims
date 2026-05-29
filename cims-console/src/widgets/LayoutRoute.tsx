// 출력 섹션 route 를 합성 가능한 EditableLayout 으로 렌더하는 범용 wrapper.
// 고정 페이지를 대체 — layout_id 별로 OAM 영속, admin 은 [✎ 편집]으로 위젯 추가/배치.
import { EditableLayout } from './EditableLayout'
import type { PageLayout } from './types'

export default function LayoutRoute({ layoutId, seed }: { layoutId: string; seed: PageLayout }) {
  return (
    <div className="page">
      <EditableLayout layoutId={layoutId} seed={seed} />
    </div>
  )
}
