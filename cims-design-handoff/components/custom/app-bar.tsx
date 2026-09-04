import { Bell, Settings, ChevronDown } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { StatusDot } from "./status-dot";

/**
 * AppBar — 상단 셸.
 *
 * 웹 대비 변경점 (D6 스펙):
 *  - 라벨 없는 아이콘 5개(편집 · <> · 테마 · 자물쇠 · 로그아웃)를 세 곳으로 정리
 *      다크 모드 · 개발자 모드 → [설정] 드롭다운의 토글 스위치
 *      비밀번호 변경 · 로그아웃 → [관리자] 계정 메뉴
 *      위젯 편집 → 제거 (재도입 시 breadcrumb 줄 오른쪽의 페이지 액션으로)
 *  - 알람 배지 신설 — 사이드바 [장애] 와 같은 카운트를 봐야 한다
 *  - 접속 환경 칩 + 호스트 + 마지막 갱신 시각 신설. fetch 실패가 쌓이면
 *    점을 danger 로 바꾸고 "갱신 실패" 로 표기 (토스트만으로는 전달되지 않음)
 *
 * 주의: 톱니바퀴 아이콘이 앱 번들에 없습니다. lucide-react 의 Settings 를 import 하세요.
 *       sliders-horizontal 은 사이드바 [메뉴 편집] 이 이미 쓰고 있어 의미가 충돌합니다.
 */
export function AppBar({
  env, host, updatedAt, alarmCount, userName, role, stale,
}: {
  env: string; host: string; updatedAt: string;
  alarmCount: number; userName: string; role: string; stale?: boolean;
}) {
  return (
    <header className="flex h-[58px] items-center gap-3 border-b border-sidebar-border bg-[var(--cims-surface-header)] px-5">
      <div className="flex items-center gap-2 font-semibold">
        <span className="text-primary">((o))</span> CIMS
      </div>

      <Badge variant="brandSoft">{env}</Badge>
      <span className="font-mono text-xs text-muted-foreground">{host}</span>
      <span className="text-xs text-muted-foreground">·</span>
      <StatusDot
        tone={stale ? "danger" : "success"}
        label={stale ? "갱신 실패" : `갱신 ${updatedAt}`}
      />

      <div className="flex-1" />

      <button className="relative flex items-center gap-1.5 rounded-md px-2 py-1.5 hover:bg-accent" aria-label={`활성 알람 ${alarmCount}건`}>
        <Bell className="size-4" />
        {alarmCount > 0 && <Badge variant="dangerSolid">{alarmCount}</Badge>}
      </button>

      <button className="rounded-md p-2 hover:bg-accent" aria-label="설정">
        <Settings className="size-4" />
      </button>

      <button className="flex items-center gap-1.5 rounded-md px-2 py-1.5 hover:bg-accent">
        <span className="text-md font-medium">{userName}</span>
        <Badge variant="brandSoft">{role}</Badge>
        <ChevronDown className="size-3.5 text-muted-foreground" />
      </button>
    </header>
  );
}
