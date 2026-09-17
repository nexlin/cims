import { AlertTriangle } from 'lucide-react'
import { Alert } from '@core/components/ui/alert'
import { usePageParam } from '@core/widgets/pageParams'
import { noticeScope, useNotices } from '@core/widgets/shapes/sourceNotice'

/**
 * 조회가 **못 본 구간**을 페이지 상단에 알린다 (sip_statistics.md §7.2).
 *
 * 왜 필요한가: 집계도 원본도 없는 날은 표에서 `—` 로 나오지만(§2.1c) **표를 훑어야 보인다.**
 * 지표 타일만 보고 "이 구간 성공률 91.7%" 로 읽는 사람은 그 값이 18일 중 10일치로 계산된
 * 것임을 알 수 없다 — 숫자를 잘못 읽게 만드는 상황이라 눈에 걸려야 한다.
 *
 * 경고가 없으면 **아무것도 그리지 않는다** — 정상일 때 화면이 시끄러워지지 않는다. 문구는
 * 서버가 낸 `warning` 그대로다(콘솔이 따로 판정하지 않는다 — 판정은 커버리지를 가진 쪽이 한다).
 * 조회 조건이 바뀌면 옛 문구는 저절로 빠진다(`noticeScope`).
 */
export default function DataNoticeBanner() {
  const [from] = usePageParam('from')
  const [to] = usePageParam('to')
  const [gran] = usePageParam('gran')
  const notices = useNotices(noticeScope(from, to, gran))
  if (notices.length === 0) return null

  return (
    <Alert variant="warning" className="mb-2 flex items-start gap-2">
      <AlertTriangle className="mt-0.5" aria-hidden="true" />
      <div className="min-w-0 leading-[1.6]">
        {notices.map((t, i) => <div key={i}>{t}</div>)}
      </div>
    </Alert>
  )
}
