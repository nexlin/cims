import { useState } from 'react'
import { ChevronDown, KeyRound, LogOut, Radio, Settings } from 'lucide-react'
import { useDevMode } from '../hooks/useDevMode'
import { setDevMode } from '../utils/devMode'
import { getInitialTheme, applyTheme, type Theme } from '../theme'
import { ROLE_LABELS, roleRank } from '../utils/permissions'
import type { Role } from '../api/auth'
import { useAlarms } from '../widgets/useAlarms'
import AlarmIndicator from './AlarmIndicator'
import { Badge } from './ui/badge'
import { Switch } from './ui/switch'
import {
  DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger,
} from './ui/dropdown-menu'
import { StatusDot } from './custom/status-dot'

/**
 * AppBar — 상단 셸. 정본 = Figma `02 Components` Sec/AppBar (457:5431) + `screens/shell.md`.
 *
 * 웹 대비 바뀐 것 (decisions.md §1 · D6 스펙):
 *  - 라벨 없는 아이콘 5개를 성격에 따라 세 곳으로 정리
 *      다크 모드 · 개발자 모드 → [설정] 드롭다운의 토글 스위치
 *      비밀번호 변경 · 로그아웃 → [계정] 메뉴
 *      위젯 편집 ✎ → **여기 남는다.** 시안 누락으로 확인됐다(정본 문서 §7-2)
 *  - 접속 환경 칩 + 호스트 + 갱신 시각 신설 — 여러 인스턴스를 운영하는데 어디에 접속했는지
 *    표시가 없었다. 환경 축은 **개발자 모드 스위치**다(켜짐=개발 / 꺼짐=운영) — 우측에 따로
 *    있던 「개발자 모드」 배지를 이 칩이 대신한다
 *  - 알람은 벨 + 카운트 배지
 *
 * 라벨을 「테마」가 아니라 「다크 모드」로 둔 이유: 스위치의 꺼짐/켜짐이 라이트/다크에
 * 대응해야 하기 때문이다 (decisions.md §1).
 */
interface HeaderProps {
  userName: string
  userRole: string
  onLogout: () => void
  onChangePw: () => void
}

/** 마지막 갱신 시각 — 셸에서 상시 도는 알람 폴링을 서버 연결의 대리 지표로 쓴다. */
function Freshness() {
  const { error, loaded, lastUpdated } = useAlarms()
  if (!loaded && !error) return <StatusDot tone="neutral" label="연결 중" />
  if (error) return <StatusDot tone="danger" label="갱신 실패" />
  // 시안 표기는 `갱신 14:27:41` — ko-KR 로케일은 "13시 30분 4초" 를 내므로 직접 24시각으로 낸다.
  const t = lastUpdated ? new Date(lastUpdated).toTimeString().slice(0, 8) : '—'
  return <StatusDot tone="success" label={`갱신 ${t}`} />
}

export default function Header({ userName, userRole, onLogout, onChangePw }: HeaderProps) {
  const [theme, setTheme] = useState<Theme>(getInitialTheme)
  const devMode = useDevMode()
  const isAdminRank = roleRank(userRole) >= roleRank('admin')
  const setDark = (on: boolean) => {
    const next: Theme = on ? 'dark' : 'light'
    applyTheme(next); setTheme(next)
  }
  return (
    // `app-header` 는 남긴다 — 그리드 배치(grid-area)와 인쇄 시 숨김 규칙이 이 이름을 쓴다.
    // 치수는 Figma AppBar(457:5430) 실측: 높이 58 · 좌우 여백 20 · 로고→환경 16 ·
    // 환경 안 8 · 유틸리티 사이 6. 구간마다 달라 한 gap 으로 묶지 않는다.
    <header className="app-header flex h-[58px] items-center border-b border-sidebar-border bg-[var(--cims-surface-header)] px-5">
      <div className="flex items-center gap-2 text-lg font-semibold tracking-tight text-[var(--cims-text-header)]">
        <Radio size={20} className="text-primary" />
        CIMS
      </div>

      {/* 접속 환경 클러스터 (시안 envCluster) — 칩 · 호스트 · 갱신 시각.
          환경은 개발자 모드 스위치가 곧 축이다: 켜져 있으면 개발, 꺼져 있으면 운영.
          브라우저 로컬(localStorage) 화면 모드라 서버 속성이 아니라 "지금 내가 보는 모드" 다. */}
      <div className="ml-4 flex items-center gap-2">
        {devMode
          ? <Badge className="bg-[var(--dev-accent)] text-white">개발</Badge>
          : <Badge variant="brandSoft">운영</Badge>}
        <span className="font-mono text-xs text-muted-foreground">{window.location.host}</span>
        <span className="text-xs text-muted-foreground">·</span>
        <Freshness />
      </div>

      {/* 페이지 위젯 편집 컨트롤 슬롯 — EditableLayout 이 portal 로 렌더.
          시안의 spacer 자리를 겸해 남은 공간을 먹고 우측 유틸리티를 밀어낸다. */}
      <div id="layout-edit-slot" className="flex min-w-0 flex-1 items-center justify-end gap-1.5 overflow-hidden px-4" />

      {/* 유틸리티 — 알람 · 설정 · 계정 (시안 utilities, 사이 6px) */}
      <div className="flex items-center gap-1.5">

      {/* 알람 — 벨 + 카운트. 드로어·전이 토스트는 AlarmIndicator 가 계속 담당한다 */}
      <AlarmIndicator />

      <DropdownMenu>
        <DropdownMenuTrigger
          className="rounded-md p-2 text-neutral hover:bg-sidebar-accent hover:text-[var(--cims-text-header)]"
          aria-label="설정">
          <Settings size={16} />
        </DropdownMenuTrigger>
        <DropdownMenuContent align="end" className="w-56">
          <DropdownMenuItem
            className="justify-between"
            onSelect={e => { e.preventDefault(); setDark(theme !== 'dark') }}>
            다크 모드
            <Switch checked={theme === 'dark'} aria-label="다크 모드" />
          </DropdownMenuItem>
          {isAdminRank && (
            <DropdownMenuItem
              className="justify-between"
              onSelect={e => { e.preventDefault(); setDevMode(!devMode) }}
              title={devMode ? '끄면 빌드·검증·패키징 메뉴가 숨겨진다' : '켜면 빌드·검증·패키징 메뉴가 나온다'}>
              개발자 모드
              <Switch checked={devMode} aria-label="개발자 모드" />
            </DropdownMenuItem>
          )}
        </DropdownMenuContent>
      </DropdownMenu>

      <DropdownMenu>
        <DropdownMenuTrigger className="flex items-center gap-1.5 rounded-md px-2 py-1.5 hover:bg-sidebar-accent">
          <span className="text-md font-medium text-[var(--cims-text-header)]">{userName}</span>
          <Badge variant={roleRank(userRole) >= 4 ? 'brandSoft' : 'neutralSoft'}>
            {ROLE_LABELS[userRole as Role] ?? userRole}
          </Badge>
          <ChevronDown size={13} className="text-muted-foreground" />
        </DropdownMenuTrigger>
        <DropdownMenuContent align="end" className="w-44">
          <DropdownMenuItem onSelect={onChangePw}>
            <KeyRound size={14} /> 비밀번호 변경
          </DropdownMenuItem>
          <DropdownMenuItem onSelect={onLogout}>
            <LogOut size={14} /> 로그아웃
          </DropdownMenuItem>
        </DropdownMenuContent>
      </DropdownMenu>
      </div>
    </header>
  )
}
