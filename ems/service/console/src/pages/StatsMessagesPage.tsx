import { useState, useEffect, useCallback } from 'react'
import { api } from '@core/api/client'
import { useToast } from '@core/components/Toast'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'
import { DataTable, Th, Td } from '@core/components/custom/data-table'

interface MsgStats {
  date: string
  total: number
  buckets: Array<{ hour: number; count: number }>
  method_counts: Record<string, number>
}

export default function StatsMessagesPage({ iface }: { iface: string }) {
  const { show } = useToast()
  const [data, setData] = useState<MsgStats | null>(null)
  const [date, setDate] = useState(new Date().toISOString().substring(0, 10))
  const [loading, setLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try { setData(await api.get<MsgStats>(`/stats/messages/${iface}?date=${date}`)) }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [iface, date, show])

  useEffect(() => { load() }, [load])

  const max = data ? Math.max(...data.buckets.map(b => b.count), 1) : 1

  return (
    <div>
      <div className="toolbar">
        <Input type="date" value={date} onChange={e => setDate(e.target.value)} style={{ width: 150 }} />
        <Button variant="default" onClick={load}>조회</Button>
        {data && <span className="ts" style={{ marginLeft: 'auto' }}>총 {data.total}건</span>}
      </div>

      {loading ? <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div> : data && (
        <div style={{ display: 'flex', gap: 24 }}>
          <div style={{ flex: 1 }}>
            <div style={{ fontWeight: 600, fontSize: 14, marginBottom: 12 }}>시간대별 메시지 수</div>
            <div style={{ display: 'flex', alignItems: 'flex-end', gap: 2, height: 200, borderBottom: '1px solid var(--border)', paddingBottom: 4 }}>
              {data.buckets.map(b => (
                <div key={b.hour} style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
                  <div style={{ width: '100%', background: 'var(--primary)', borderRadius: '2px 2px 0 0',
                    height: `${(b.count / max) * 180}px`, minHeight: b.count > 0 ? 2 : 0 }}
                    title={`${b.hour}시: ${b.count}건`} />
                </div>
              ))}
            </div>
            <div style={{ display: 'flex', gap: 2, fontSize: 9, color: 'var(--muted-foreground)', marginTop: 2 }}>
              {data.buckets.map(b => <div key={b.hour} style={{ flex: 1, textAlign: 'center' }}>{b.hour}</div>)}
            </div>
          </div>

          <div style={{ width: 300 }}>
            <div style={{ fontWeight: 600, fontSize: 14, marginBottom: 12 }}>메서드별 카운트</div>
            <DataTable sticky>
              <thead><tr><Th>메서드</Th><Th style={{ width: 80 }}>건수</Th></tr></thead>
              <tbody>
                {Object.entries(data.method_counts).map(([m, c]) => (
                  <tr key={m}><Td style={{ fontSize: 12 }}>{m}</Td><Td style={{ fontSize: 12, textAlign: 'right', fontWeight: 600 }}>{c}</Td></tr>
                ))}
                {Object.keys(data.method_counts).length === 0 && <tr><Td colSpan={2} className="py-8 text-center text-muted-foreground">데이터 없음</Td></tr>}
              </tbody>
            </DataTable>
          </div>
        </div>
      )}
    </div>
  )
}
