import { cn } from "@/lib/utils";

/**
 * EmptyState — 빈 상태 5종에 모두 이걸 씁니다 (screens/empty-states.md).
 * 화면마다 다른 문구·다른 모양을 만들지 마세요.
 */
export function EmptyState({
  title,
  description,
  action,
  className,
}: {
  title: string;
  description?: string;
  action?: React.ReactNode;
  className?: string;
}) {
  return (
    <div
      className={cn(
        "flex flex-col items-center gap-1.5 rounded-md border border-dashed border-border bg-muted/40 px-6 py-8 text-center",
        className,
      )}
    >
      <p className="text-base font-semibold text-foreground">{title}</p>
      {description && (
        <p className="max-w-[52ch] text-xs text-muted-foreground">{description}</p>
      )}
      {action && <div className="mt-2">{action}</div>}
    </div>
  );
}
