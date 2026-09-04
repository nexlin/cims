import { cn } from "@/lib/utils";

/**
 * StatusDot — 살아있는 상태 표시.
 * 값·분류는 Badge 를 쓰고, 프로세스/노드의 현재 상태만 여기로.
 * 톤 매핑은 고정입니다 (DESIGN-RULES.md §2).
 */
export type StatusTone = "success" | "info" | "neutral" | "warning" | "danger";

const TONE: Record<StatusTone, string> = {
  success: "bg-success",
  info: "bg-info",
  neutral: "bg-neutral",
  warning: "bg-warning",
  danger: "bg-destructive",
};

/** 서버/모듈 상태 문자열 → 톤. 새 상태가 생기면 여기만 고치세요. */
export function toneForStatus(status: string): StatusTone {
  switch (status) {
    case "online":
    case "running":
    case "mounted":
    case "Active":
      return "success";
    case "approved":
      return "info";
    case "stopped":
    case "Standby":
    case "unset":
      return "neutral";
    case "drift":
      return "warning";
    default:
      return "danger"; // offline · unreachable · critical
  }
}

export function StatusDot({
  status,
  label,
  tone,
  className,
}: {
  status?: string;
  label?: string;
  tone?: StatusTone;
  className?: string;
}) {
  const t = tone ?? toneForStatus(status ?? "");
  return (
    <span className={cn("inline-flex items-center gap-1.5 whitespace-nowrap", className)}>
      <span className={cn("size-1.5 shrink-0 rounded-full", TONE[t])} aria-hidden />
      <span className="text-sm text-muted-foreground">{label ?? status}</span>
    </span>
  );
}
