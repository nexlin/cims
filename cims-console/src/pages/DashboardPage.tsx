// 대시보드 = 위젯 합성 page. 코어/서비스 위젯을 layout(DASHBOARD_LAYOUT) 대로 배치.
// 개별 화면 구현은 widgets/* 와 services/cims/widgets/* 로 분리 (OAM 플랫폼화 5-3).
// 향후 layout 을 file_store 에서 로드 + 관리자 편집 (step2).
import { GridRenderer } from '../widgets/GridRenderer'
import { DASHBOARD_LAYOUT } from '../widgets/layouts'

export default function DashboardPage() {
  return (
    <div className="page">
      <GridRenderer layout={DASHBOARD_LAYOUT} />
    </div>
  )
}
