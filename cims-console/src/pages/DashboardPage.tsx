// 대시보드 = 편집 가능한 위젯 합성 page. 저장된 layout 로드(없으면 seed), admin 은 [✎ 편집].
// 위젯 구현은 widgets/* 와 services/cims/widgets/* (OAM 플랫폼화 5-3).
import { EditableLayout } from '../widgets/EditableLayout'
import { DASHBOARD_LAYOUT } from '../widgets/layouts'

export default function DashboardPage() {
  return (
    <div className="page">
      <EditableLayout layoutId="dashboard" seed={DASHBOARD_LAYOUT} />
    </div>
  )
}
