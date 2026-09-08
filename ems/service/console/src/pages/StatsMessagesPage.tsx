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
        <Input className="w-[150px]" type="date" value={date} onChange={e => setDate(e.target.value)}/>
        <Button variant="default" onClick={load}>조회</Button>
        {data && <span className="text-sm text-muted-foreground ml-auto">총 {data.total}건</span>}
      </div>

      {loading ? <div className="flex min-h-0 flex-1 items-center justify-center p-8 text-center text-muted-foreground">로딩 중...</div> : data && (
        <div className="flex gap-6">
          <div className="flex-1">
            <div className="font-semibold text-base mb-3">시간대별 메시지 수</div>
            <div className="flex items-end gap-0.5 h-[200px] border-b border-border pb-1">
              {data.buckets.map(b => (
                <div className="flex-1 flex flex-col items-center" key={b.hour}>
                  <div style={{ width: '100%', background: 'var(--primary)', borderRadius: '2px 2px 0 0',
                    height: `${(b.count / max) * 180}px`, minHeight: b.count > 0 ? 2 : 0 }}
                    title={`${b.hour}시: ${b.count}건`} />
                </div>
              ))}
            </div>
            <div className="flex gap-0.5 text-[9px] text-muted-foreground mt-0.5">
              {data.buckets.map(b => <div className="flex-1 text-center" key={b.hour}>{b.hour}</div>)}
            </div>
          </div>

          <div className="w-[300px]">
            <div className="font-semibold text-base mb-3">메서드별 카운트</div>
            <DataTable sticky>
              <thead><tr><Th>메서드</Th><Th className="w-[80px]">건수</Th></tr></thead>
              <tbody>
                {Object.entries(data.method_counts).map(([m, c]) => (
                  <tr key={m}><Td className="text-sm">{m}</Td><Td className="text-sm text-right font-semibold">{c}</Td></tr>
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
