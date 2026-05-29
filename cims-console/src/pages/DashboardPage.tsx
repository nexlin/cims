// 대시보드 = 위젯 합성 page. 저장된 layout(OAM file_store) 로드, 없으면 seed.
// 개별 화면 구현은 widgets/* 와 services/cims/widgets/* (OAM 플랫폼화 5-3).
import { GridRenderer } from '../widgets/GridRenderer'
import { DASHBOARD_LAYOUT } from '../widgets/layouts'
import { useStoredLayout } from '../widgets/useStoredLayout'

export default function DashboardPage() {
  const layout = useStoredLayout(DASHBOARD_LAYOUT)
  return (
    <div className="page">
      <GridRenderer layout={layout} />
    </div>
  )
}
