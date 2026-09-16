// 계측기(oam-cims-tester) 콘솔 팩 — 콘솔 기여 매니페스트 (test_instrument.md §7).
//
// 관리 영역에 그룹 `test`(시험) 하나 — ITU-T M.3400 Maintenance 의 Testing 기능군. 릴리스(검증
// 게이트)와 다르다. 섹션은 requiresService='oam-cims-tester' — 그 모듈이 게이트웨이에 등록돼
// 있을 때만 사이드바에 나타난다(MenuContext 게이팅). 화면·API 는 전부 이 팩 소유, 코어 수정 없음.
import { FlaskConical } from 'lucide-react'
import type { ServiceManifest } from '@core/nav-types'

import TesterRunsPage from './pages/TesterRunsPage'
import TesterScenariosPage from './pages/TesterScenariosPage'

export const testerManifest: ServiceManifest = {
  id: 'tester',
  label: '계측기',
  sections: [
    {
      key: 'test',
      label: '시험',
      icon: FlaskConical,
      area: 'admin',
      basePath: '/test',
      defaultPath: '/test/runs',
      order: 75,             // 릴리스(70) 다음, 문서(90) 앞
      requiresService: 'oam-cims-tester',
      routes: [
        { path: '/test/runs',      title: '실행',       component: TesterRunsPage,      requiredRole: 'monitor',
          apis: ['tester.health', 'tester.runs', 'tester.events'] },
        { path: '/test/scenarios', title: '시나리오',   component: TesterScenariosPage, requiredRole: 'monitor',
          apis: ['tester.scenarios', 'tester.profiles'] },
      ],
    },
  ],
}
