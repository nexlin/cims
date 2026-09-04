import { ChevronDown, Plus, X, Search } from "lucide-react";
import { cn } from "@/lib/utils";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { StatusDot, toneForStatus } from "./status-dot";

/**
 * TreePanel — 좌측 시스템 트리.
 * hasControl 은 AA(all_active) 그룹에서만 켭니다:
 *   그룹 행 = + (새 멤버 자동 생성) · 멤버 행 = × (그룹에서 제거, agent 는 standalone 유지)
 * AS 는 서버 2대 고정이라 항상 끕니다.
 */
export type TreeNode =
  | { kind: "group"; id: string; role: "AS" | "AA" | "SA"; label: string; count: number; children: TreeNode[] }
  | { kind: "node"; id: string; label: string; status: string; meta: string };

export function TreePanel({
  systems,
  serverCount,
  selectedId,
  onSelect,
  onAddMember,
  onRemoveMember,
  onCreateSystem,
}: {
  systems: TreeNode[];
  serverCount: number;
  selectedId?: string;
  onSelect?: (id: string) => void;
  onAddMember?: (groupId: string) => void;
  onRemoveMember?: (nodeId: string) => void;
  onCreateSystem?: () => void;
}) {
  return (
    <aside className="flex w-[300px] shrink-0 flex-col rounded-md border border-border bg-card">
      <div className="flex items-baseline gap-2 px-3.5 pb-1 pt-3">
        <h2 className="text-base font-semibold">시스템</h2>
        <span className="text-xs text-muted-foreground">
          시스템 {systems.length} · 서버 {serverCount}
        </span>
      </div>

      <div className="relative px-2.5 pb-2.5">
        <Search className="pointer-events-none absolute left-5 top-1/2 size-3.5 -translate-y-1/2 text-muted-foreground" />
        <input
          className="h-[34px] w-full rounded-md border border-input bg-background pl-8 pr-2 text-md placeholder:text-muted-foreground"
          placeholder="서버 이름·IP 검색"
        />
      </div>

      <div className="flex-1 overflow-y-auto px-2.5 pb-2.5">
        {systems.map((sys) =>
          sys.kind !== "group" ? null : (
            <div key={sys.id}>
              <TreeRow
                selected={selectedId === sys.id}
                onClick={() => onSelect?.(sys.id)}
                left={
                  <>
                    <ChevronDown className="size-3.5 text-muted-foreground" />
                    <Badge variant="brandSoft" className="px-1 py-0 text-[10px] leading-4">
                      {sys.role}
                    </Badge>
                  </>
                }
                label={sys.label}
                meta={String(sys.count)}
                control={
                  sys.role === "AA" && (
                    <IconBtn title="새 멤버 자동 생성 (이름 자동, install_command 발급)" onClick={() => onAddMember?.(sys.id)}>
                      <Plus className="size-3.5" />
                    </IconBtn>
                  )
                }
              />
              {sys.children.map((n) =>
                n.kind !== "node" ? null : (
                  <TreeRow
                    key={n.id}
                    indent
                    selected={selectedId === n.id}
                    onClick={() => onSelect?.(n.id)}
                    left={<span className={cn("ml-2.5 size-1.5 rounded-full", dotClass(n.status))} />}
                    label={n.label}
                    meta={n.meta}
                    control={
                      sys.role === "AA" && (
                        <IconBtn title="그룹에서 멤버 제거 (agent 자체는 standalone 으로 유지)" onClick={() => onRemoveMember?.(n.id)}>
                          <X className="size-3.5" />
                        </IconBtn>
                      )
                    }
                  />
                ),
              )}
            </div>
          ),
        )}
      </div>

      <div className="p-2.5">
        <Button className="w-full" onClick={onCreateSystem}>
          <Plus className="size-4" /> 시스템 추가
        </Button>
      </div>
    </aside>
  );
}

function dotClass(status: string) {
  const t = toneForStatus(status);
  return { success: "bg-success", info: "bg-info", neutral: "bg-neutral", warning: "bg-warning", danger: "bg-destructive" }[t];
}

function IconBtn({ title, onClick, children }: { title: string; onClick?: () => void; children: React.ReactNode }) {
  return (
    <button
      type="button"
      title={title}
      aria-label={title}
      onClick={(e) => { e.stopPropagation(); onClick?.(); }}
      className="rounded p-0.5 text-muted-foreground hover:bg-accent hover:text-foreground"
    >
      {children}
    </button>
  );
}

function TreeRow({
  left, label, meta, control, selected, indent, onClick,
}: {
  left?: React.ReactNode; label: string; meta?: string; control?: React.ReactNode;
  selected?: boolean; indent?: boolean; onClick?: () => void;
}) {
  return (
    <div
      role="treeitem"
      aria-selected={selected}
      tabIndex={0}
      onClick={onClick}
      className={cn(
        "flex h-8 cursor-pointer items-center gap-1.5 rounded-md px-2 text-md",
        selected ? "bg-accent font-medium text-primary" : "hover:bg-accent/60",
        indent && "pl-2",
      )}
    >
      {left}
      <span className="flex-1 truncate">{label}</span>
      {meta && <span className="text-xs text-muted-foreground">{meta}</span>}
      {control}
    </div>
  );
}
