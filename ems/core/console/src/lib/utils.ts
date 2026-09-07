import { clsx, type ClassValue } from 'clsx'
import { twMerge } from 'tailwind-merge'

/** shadcn 표준 헬퍼 — 조건부 클래스 합성 + Tailwind 충돌 해소. */
export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs))
}
