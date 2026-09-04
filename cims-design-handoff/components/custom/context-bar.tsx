import { Pencil, ChevronDown } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { StatusDot } from "./status-dot";

/**
 * ContextBar — 지금 보고 있는 객체가 무엇인지 알려주는 줄.
 *
 * 웹과 다른 점(의도된 변경 — 핸드오프 시 개발자에게 반드시 알릴 것):
 *  - 웹은 그룹 스코프에서 이 줄이 1번 탭에만 있고 나머지 3탭에서는 사라진다.
 *    → 4탭 전부에 유지한다. 어느 그룹인지 · vrid · 삭제/비교 진입점이 사라지면 안 된다.
 *  - 서버 스코프의 재시작/업그레이드/롤백/폐기/삭제는 [더보기] 드롭다운으로 묶는다.
 */
export function ServerContextBar({
  name, status, id, version, onRename, actions,
}: {
  name: string; status: string; id: string; version: string;
  onRename?: () => void; actions?: React.ReactNode;
}) {
  return (
    <div className="flex h-[60px] items-center gap-2 rounded-md border border-border bg-card px-4">
      <StatusDot status={status} label="" />
      <span className="text-lg font-semibold">{name}</span>
      <button
        type="button"
        title="서버 이름 변경 (표시용 — 시스템은 #id 로 동작)"
        aria-label="서버 이름 변경"
        onClick={onRename}
        className="rounded p-1 text-muted-foreground hover:bg-accent hover:text-foreground"
      >
        <Pencil className="size-3.5" />
      </button>
      <Badge variant="successSoft">{status}</Badge>
      <span className="font-mono text-xs text-muted-foreground">
        {id} · {version}
      </span>
      <div className="flex-1" />
      {actions}
    </div>
  );
}

export function GroupContextBar({
  role, name, nodeCount, id, vrid, actions,
}: {
  role: "AS" | "AA"; name: string; nodeCount: number; id: string; vrid: number; actions?: React.ReactNode;
}) {
  return (
    <div className="flex h-[60px] items-center gap-2 rounded-md border border-border bg-card px-4">
      <Badge variant="brandSoft">{role}</Badge>
      <span className="text-lg font-semibold">{name}</span>
      <Badge variant="neutralSoft">그룹 · 노드 {nodeCount}</Badge>
      <span className="font-mono text-xs text-muted-foreground">
        {id} · vrid {vrid}
      </span>
      <div className="flex-1" />
      {actions}
    </div>
  );
}

export function MoreButton({ children }: { children?: React.ReactNode }) {
  return (
    <Button variant="outline" size="sm">
      더보기 <ChevronDown className="size-3.5" />
      {children}
    </Button>
  );
}
