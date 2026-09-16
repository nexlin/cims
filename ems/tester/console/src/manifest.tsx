// 계측기(oam-cims-tester) 콘솔 팩 — 콘솔 기여 매니페스트 (test_instrument.md §7).
//
// 관리 영역에 그룹 `test`(시험) 하나 — ITU-T M.3400 Maintenance 의 Testing 기능군. 릴리스(검증
// 게이트)와 다르다. 섹션은 requiresService='oam-cims-tester' — 그 모듈이 게이트웨이에 등록돼
// 있을 때만 사이드바에 나타난다(MenuContext 게이팅). 화면·API 는 전부 이 팩 소유, 코어 수정 없음.
// `apis` 는 개발자 모드 [API] 배지 — id 는 handlers/tester.py 의 TESTER_API_DOCS 와 같다.
import { FlaskConical } from 'lucide-react'
import type { ServiceManifest } from '@core/nav-types'

import TesterRunsPage from './pages/TesterRunsPage'
import TesterResultsPage from './pages/TesterResultsPage'
import TesterComparePage from './pages/TesterComparePage'
import TesterScenariosPage from './pages/TesterScenariosPage'
import TesterTopologiesPage from './pages/TesterTopologiesPage'

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
        { path: '/test/runs',       title: '실행',       component: TesterRunsPage,       requiredRole: 'monitor',
          apis: ['tester.health', 'tester.runs', 'tester.run.start', 'tester.runs.plan', 'tester.run.stop', 'tester.run.rate', 'tester.run.hold', 'tester.run.stream', 'tester.run.series', 'tester.run.events', 'tester.run.sip', 'tester.events', 'tester.workers', 'tester.topologies', 'tester.scenario'] },
        { path: '/test/results',    title: '결과',       component: TesterResultsPage,    requiredRole: 'monitor',
          apis: ['tester.runs', 'tester.run.report', 'tester.run.events', 'tester.run.series', 'tester.run.hist', 'tester.run.sip', 'tester.run.target_alerts', 'tester.run.delete', 'tester.scenario', 'tester.runs.plan'] },
        { path: '/test/compare',    title: '비교',       component: TesterComparePage,    requiredRole: 'monitor',
          apis: ['tester.runs', 'tester.runs.compare', 'tester.run.series', 'tester.scenario'] },
        { path: '/test/scenarios',  title: '시나리오',   component: TesterScenariosPage,  requiredRole: 'monitor',
          apis: ['tester.scenarios', 'tester.scenario', 'tester.scenario.put', 'tester.scenario.delete', 'tester.scenarios.vocab', 'tester.scenarios.compile_check', 'tester.profiles', 'tester.profile', 'tester.validate', 'tester.topologies', 'tester.run.start'] },
        { path: '/test/topologies', title: '토폴로지',   component: TesterTopologiesPage, requiredRole: 'monitor',
          apis: ['tester.topologies', 'tester.topology.save', 'tester.topology.check', 'tester.workers', 'tester.validate'] },
      ],
    },
  ],
}
