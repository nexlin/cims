import { AlertTriangle, RefreshCw } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { StatusDot } from "./status-dot";

/**
 * SyncStatusRow — 그룹 › 패키지 설정 상단 상태 줄.
 * 드리프트가 0건이면 hasDrift 를 false 로. 0건에 경고색을 쓰지 마세요.
 */
export function SyncStatusRow({
  syncOn,
  activeNode,
  driftCount = 0,
  onRefresh,
}: {
  syncOn: boolean;
  activeNode: string;
  driftCount?: number;
  onRefresh?: () => void;
}) {
  const hasDrift = driftCount > 0;
  return (
    <div className="flex items-center gap-3">
      <Badge variant={syncOn ? "successSoft" : "neutralSoft"}>
        동기화 {syncOn ? "ON" : "OFF"}
      </Badge>
      <StatusDot status="online" label={`ACTIVE ${activeNode}`} />
      {hasDrift && (
        <>
          <span className="text-xs text-muted-foreground">·</span>
          <span className="inline-flex items-center gap-1 text-xs text-warning-on">
            <AlertTriangle className="size-3.5" />
            드리프트 {driftCount}건 — 자동 교정 대기 중
          </span>
        </>
      )}
      <div className="flex-1" />
      <Button variant="ghost" size="sm" onClick={onRefresh}>
        <RefreshCw className="size-3.5" />
        새로고침
      </Button>
    </div>
  );
}
