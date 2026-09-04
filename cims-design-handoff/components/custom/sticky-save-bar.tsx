import { Undo2, Check } from "lucide-react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";

/**
 * StickySaveBar — 폼 하단 저장바.
 * `되돌리기` 는 현재 웹에 없는 제안 항목입니다. 채택 여부를 확인하고 쓰세요.
 */
export function StickySaveBar({
  changeCount,
  badgeLabel,
  note,
  saveLabel = "저장",
  onRevert,
  onSave,
}: {
  changeCount: number;
  badgeLabel?: string;
  note: string;
  saveLabel?: string;
  onRevert?: () => void;
  onSave?: () => void;
}) {
  return (
    <div className="sticky bottom-0 flex items-center gap-3 border-t border-border bg-card px-5 py-3">
      <Badge variant="neutralSoft">{badgeLabel ?? `변경 ${changeCount}건`}</Badge>
      <p className="text-xs text-muted-foreground">{note}</p>
      <div className="flex-1" />
      <Button variant="ghost" size="sm" onClick={onRevert} disabled={changeCount === 0}>
        <Undo2 className="size-3.5" />
        되돌리기
      </Button>
      <Button size="sm" onClick={onSave} disabled={changeCount === 0}>
        <Check className="size-3.5" />
        {saveLabel} ({changeCount} 변경)
      </Button>
    </div>
  );
}
