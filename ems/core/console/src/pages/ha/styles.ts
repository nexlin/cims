import type { CSSProperties } from 'react'

export function btnSmall(): CSSProperties {
  return { fontSize: 11, padding: '2px 8px', marginLeft: 4, cursor: 'pointer',
           background: 'var(--card)', border: '1px solid var(--border)', borderRadius: 3 }
}
export function btnPrimary(): CSSProperties {
  return { fontSize: 12, padding: '4px 12px', marginRight: 4, cursor: 'pointer',
           background: '#3498db', color: '#fff', border: 'none', borderRadius: 3 }
}
export function btnSecondary(): CSSProperties {
  return { fontSize: 12, padding: '4px 12px', cursor: 'pointer',
           background: 'var(--card)', border: '1px solid var(--border)', borderRadius: 3 }
}
export function btnDanger(): CSSProperties {
  return { fontSize: 11, padding: '2px 8px', cursor: 'pointer',
           background: 'var(--card)', border: '1px solid #c0392b', color: 'var(--destructive)', borderRadius: 3 }
}
export function btnAdd(small = false): CSSProperties {
  return { fontSize: small ? 11 : 13, padding: small ? '3px 10px' : '6px 16px',
           cursor: 'pointer', background: 'var(--cims-brand-soft)', border: '1px dashed #3498db',
           color: '#3498db', borderRadius: 3 }
}
