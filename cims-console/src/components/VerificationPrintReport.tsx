// ─────────────────────────────────────────────────────────────
// 검증 PrintReport — print 시에만 보임 (display:none 기본).
// V2 페이지 LIVE 결과 + 이력 페이지 detail 모두 동일 컴포넌트로 PDF 출력.
//
// 사용:
//   <VerificationPrintReport stages={...} resumeStage={1} reportMeta={...} />
//   window.print() 시 v2-report 클래스가 .v2-report 인쇄 스타일에 의해 노출.
// ─────────────────────────────────────────────────────────────

export type ItemStatus = 'PENDING' | 'RUNNING' | 'PASS' | 'FAIL' | 'SKIP' | 'BLOCKED'

export interface ReportItem {
  id: string
  name: string
  desc?: string
  status: ItemStatus
  elapsedMs: number
  isGroup?: boolean
  parent?: string
}

export interface ReportStage {
  num: number
  id: string
  title: string
  desc: string
  items: ReportItem[]
}

export interface ReportMeta {
  /** 발행 일시 (없으면 new Date().toLocaleString('ko-KR')) */
  issuedAt?: string
  host?: string
  gitBranch?: string
  gitSha?: string
  pkgManifest?: string
  /** 회차 번호 (history detail) — 없으면 LIVE 보고서 */
  runId?: number
}

// 표시 helper
export function statusIcon(s: ItemStatus): string {
  if (s === 'PASS')    return '✅'
  if (s === 'FAIL')    return '❌'
  if (s === 'SKIP')    return '⏭'
  if (s === 'RUNNING') return '⏳'
  if (s === 'BLOCKED') return '🚫'
  return '⏸'
}

export function statusLabel(s: ItemStatus): string {
  if (s === 'PASS')    return '성공'
  if (s === 'FAIL')    return '실패'
  if (s === 'SKIP')    return '건너뜀'
  if (s === 'RUNNING') return '진행중'
  if (s === 'BLOCKED') return '차단'
  return '대기'
}

export function statusColor(s: ItemStatus): string {
  if (s === 'PASS')    return '#16a34a'
  if (s === 'FAIL')    return '#dc2626'
  if (s === 'RUNNING') return '#3b82f6'
  if (s === 'BLOCKED') return '#a16207'
  if (s === 'SKIP')    return '#6b7280'
  return '#9ca3af'
}

export function fmtMs(ms: number): string {
  if (ms < 1000) return `${ms}ms`
  if (ms < 60000) return `${(ms / 1000).toFixed(1)}s`
  const m = Math.floor(ms / 60000)
  const s = Math.floor((ms % 60000) / 1000)
  return `${m}m ${s}s`
}

export function stageStatus(items: ReportItem[]): ItemStatus {
  const flat = items.filter(it => !it.isGroup)
  if (flat.length === 0) return 'PENDING'
  if (flat.some(it => it.status === 'FAIL'))    return 'FAIL'
  if (flat.some(it => it.status === 'RUNNING')) return 'RUNNING'
  if (flat.some(it => it.status === 'BLOCKED') && !flat.some(it => it.status === 'PENDING')) return 'BLOCKED'
  if (flat.every(it => it.status === 'PASS' || it.status === 'SKIP')) return 'PASS'
  if (flat.some(it => it.status === 'PASS') && flat.some(it => it.status === 'PENDING')) return 'RUNNING'
  return 'PENDING'
}

export function stageProgress(items: ReportItem[]): { done: number; total: number; elapsed: number } {
  const flat = items.filter(it => !it.isGroup)
  const total = flat.length
  const done = flat.filter(it => it.status === 'PASS' || it.status === 'FAIL' || it.status === 'SKIP').length
  const elapsed = flat.reduce((sum, it) => sum + it.elapsedMs, 0)
  return { done, total, elapsed }
}

function groupStatus(stage: ReportStage, groupId: string): ItemStatus {
  const children = stage.items.filter(it => it.parent === groupId)
  if (children.length === 0) return 'PENDING'
  if (children.some(c => c.status === 'FAIL'))    return 'FAIL'
  if (children.some(c => c.status === 'RUNNING')) return 'RUNNING'
  if (children.every(c => c.status === 'PASS' || c.status === 'SKIP')) return 'PASS'
  if (children.some(c => c.status === 'PASS')) return 'RUNNING'
  return 'PENDING'
}

export function VerificationPrintReport({
  stages, resumeStage = 1, meta = {},
}: {
  stages: ReportStage[]
  resumeStage?: number
  meta?: ReportMeta
}) {
  const now = meta.issuedAt || new Date().toLocaleString('ko-KR')
  const overallStatus: ItemStatus = (() => {
    const sts = stages.map(s => stageStatus(s.items))
    if (sts.some(s => s === 'FAIL'))    return 'FAIL'
    if (sts.some(s => s === 'RUNNING')) return 'RUNNING'
    if (sts.every(s => s === 'PASS'))   return 'PASS'
    return 'PENDING'
  })()
  const flatItems = stages.flatMap(st => st.items.filter(it => !it.isGroup).map(it => ({ st, it })))
  const totalItems = flatItems.length
  const passCount  = flatItems.filter(({ it }) => it.status === 'PASS').length
  const failCount  = flatItems.filter(({ it }) => it.status === 'FAIL').length
  const skipCount  = flatItems.filter(({ it }) => it.status === 'SKIP').length
  const blockCount = flatItems.filter(({ it }) => it.status === 'BLOCKED').length
  const pendCount  = flatItems.filter(({ it }) => it.status === 'PENDING').length
  const totalElapsed = flatItems.reduce((s, { it }) => s + it.elapsedMs, 0)

  const cell = (text: string | number, opts: React.CSSProperties = {}) => (
    <td style={{ padding: '5px 8px', borderBottom: '1px solid #e5e7eb', ...opts }}>{text}</td>
  )
  const headCell = (text: string, opts: React.CSSProperties = {}) => (
    <th style={{ padding: '6px 8px', borderBottom: '1.5px solid #111', textAlign: 'left', background: '#f3f4f6', ...opts }}>{text}</th>
  )
  const sectionH = (color = '#111'): React.CSSProperties => ({
    fontSize: 16, fontWeight: 800, marginTop: 18, marginBottom: 8, color,
    borderLeft: `4px solid ${color}`, paddingLeft: 8,
  })

  return (
    <div className="v2-report" style={{ display: 'none', fontFamily: 'sans-serif', color: '#111' }}>
      {/* 표지 */}
      <div style={{ borderBottom: '3px double #111', paddingBottom: 16, marginBottom: 20 }}>
        <div style={{ fontSize: 11, color: '#6b7280', marginBottom: 4 }}>
          CIMS Verification Report{meta.runId !== undefined ? ` — 회차 #${meta.runId}` : ''}
        </div>
        <h1 style={{ margin: 0, fontSize: 26, fontWeight: 900, letterSpacing: -0.5 }}>
          CIMS 검증 보고서
        </h1>
        <div style={{ marginTop: 12, fontSize: 11, lineHeight: 1.8, columnCount: 2 }}>
          <div><b>발행 일시:</b> {now}</div>
          <div><b>호스트:</b> {meta.host || '-'}</div>
          <div><b>git 브랜치:</b> {meta.gitBranch || '-'}</div>
          <div><b>git revision:</b> {meta.gitSha || '-'}</div>
          <div><b>패키지 manifest:</b> <code>{meta.pkgManifest || '-'}</code></div>
          <div><b>재개 지점:</b> S{resumeStage} 단계부터</div>
        </div>
      </div>

      {/* 1. 검증결과 요약 */}
      <h2 style={sectionH()}>1. 검증결과 요약</h2>
      <div style={{ fontSize: 12, marginBottom: 12 }}>
        본 검증은 6단계 파이프라인 (S1 정적검사 → S2 빌드 → S3 스모크 → S4 패키지화 → S5 로컬배포 → S6 통합검증) 으로
        구성된 절차에 따라 수행되었습니다. 각 단계는 이전 단계의 PASS 를 전제로 진행되며,
        S4 의 패키지 manifest hash 가 S6 검증 시점과 매칭되어 빌드 산출물의 무결성(immutability)을 보장합니다.
      </div>

      {/* 종합 판정 박스 */}
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12, marginBottom: 12 }}>
        <tbody>
          <tr>
            <td style={{
              padding: '12px 16px', width: '30%',
              background: `${statusColor(overallStatus)}15`,
              border: `2px solid ${statusColor(overallStatus)}`,
              fontSize: 18, fontWeight: 800, color: statusColor(overallStatus),
              textAlign: 'center', verticalAlign: 'middle',
            }}>
              {statusIcon(overallStatus)} {statusLabel(overallStatus)}
            </td>
            <td style={{ padding: '12px 16px', border: '1px solid #d1d5db' }}>
              <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 6, fontSize: 11 }}>
                <div><b>전체 항목:</b> {totalItems}건</div>
                <div><b>총 소요시간:</b> {fmtMs(totalElapsed)}</div>
                <div><b>완료율:</b> {totalItems > 0 ? Math.round((passCount + failCount + skipCount) / totalItems * 100) : 0}%</div>
                <div style={{ color: statusColor('PASS') }}><b>✅ 성공:</b> {passCount}건</div>
                <div style={{ color: statusColor('FAIL') }}><b>❌ 실패:</b> {failCount}건</div>
                <div style={{ color: statusColor('SKIP') }}><b>⏭ 건너뜀:</b> {skipCount}건</div>
                <div style={{ color: statusColor('BLOCKED') }}><b>🚫 차단:</b> {blockCount}건</div>
                <div style={{ color: statusColor('PENDING') }}><b>⏸ 대기:</b> {pendCount}건</div>
              </div>
            </td>
          </tr>
        </tbody>
      </table>

      {/* 단계별 요약 표 */}
      <div style={{ fontSize: 12, fontWeight: 700, marginBottom: 6 }}>1.1 단계별 요약</div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11, marginBottom: 12 }}>
        <thead>
          <tr>
            {headCell('Stage', { width: 50 })}
            {headCell('단계명', { width: 110 })}
            {headCell('범위 / Gate')}
            {headCell('판정', { width: 80, textAlign: 'center' })}
            {headCell('완료/전체', { width: 80, textAlign: 'right' })}
            {headCell('소요', { width: 70, textAlign: 'right' })}
          </tr>
        </thead>
        <tbody>
          {stages.map(st => {
            const s = stageStatus(st.items)
            const { done, total, elapsed } = stageProgress(st.items)
            return (
              <tr key={st.id}>
                {cell(st.id, { fontWeight: 700 })}
                {cell(st.title, { fontWeight: 600 })}
                {cell(st.desc, { color: '#4b5563', fontSize: 10 })}
                {cell(`${statusIcon(s)} ${statusLabel(s)}`, { textAlign: 'center', color: statusColor(s), fontWeight: 700 })}
                {cell(`${done} / ${total}`, { textAlign: 'right' })}
                {cell(fmtMs(elapsed), { textAlign: 'right' })}
              </tr>
            )
          })}
        </tbody>
      </table>

      {/* 실패 항목 강조 (있을 때만) */}
      {failCount > 0 && (
        <>
          <div style={{ fontSize: 12, fontWeight: 700, marginBottom: 6, color: '#dc2626' }}>
            1.2 실패 항목 ({failCount}건) — 우선 조치 필요
          </div>
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11, marginBottom: 12 }}>
            <thead>
              <tr>
                {headCell('Stage', { width: 50, background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('항목 ID', { width: 220, background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('항목명', { background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('설명', { background: '#fef2f2', borderBottom: '1.5px solid #dc2626' })}
                {headCell('소요', { width: 60, background: '#fef2f2', borderBottom: '1.5px solid #dc2626', textAlign: 'right' })}
              </tr>
            </thead>
            <tbody>
              {flatItems.filter(({ it }) => it.status === 'FAIL').map(({ st, it }) => (
                <tr key={it.id}>
                  {cell(st.id, { fontWeight: 700 })}
                  {cell(it.id, { fontFamily: 'monospace', color: '#dc2626', fontWeight: 600 })}
                  {cell(it.name)}
                  {cell(it.desc || '—', { color: '#4b5563', fontSize: 10 })}
                  {cell(fmtMs(it.elapsedMs), { textAlign: 'right' })}
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}

      {/* 2. 검증항목별 결과 */}
      <h2 style={sectionH()}>2. 검증항목별 결과</h2>
      <div style={{ fontSize: 11, color: '#4b5563', marginBottom: 6 }}>
        전체 {totalItems}개 항목의 단순 결과 표 (그룹 자식 포함, 그룹 자체는 자식의 worst-status 반영).
      </div>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 10, marginBottom: 12 }}>
        <thead>
          <tr>
            {headCell('#', { width: 30, textAlign: 'right' })}
            {headCell('Stage', { width: 50 })}
            {headCell('항목 ID', { width: 240 })}
            {headCell('항목명')}
            {headCell('판정', { width: 75, textAlign: 'center' })}
            {headCell('소요', { width: 60, textAlign: 'right' })}
          </tr>
        </thead>
        <tbody>
          {flatItems.map(({ st, it }, idx) => (
            <tr key={it.id}>
              {cell(idx + 1, { textAlign: 'right', color: '#9ca3af' })}
              {cell(st.id, { fontWeight: 600 })}
              {cell(it.id, { fontFamily: 'monospace', fontSize: 9, paddingLeft: it.parent ? 24 : 8 })}
              {cell(it.name, { fontSize: 10 })}
              {cell(`${statusIcon(it.status)} ${statusLabel(it.status)}`, {
                textAlign: 'center', color: statusColor(it.status), fontWeight: 600,
              })}
              {cell(fmtMs(it.elapsedMs), { textAlign: 'right', color: '#6b7280' })}
            </tr>
          ))}
        </tbody>
      </table>

      {/* 3. 검증 상세내용 */}
      <h2 style={sectionH()}>3. 검증 상세내용</h2>
      <div style={{ fontSize: 11, color: '#4b5563', marginBottom: 8 }}>
        각 단계별 항목의 검증 의도 / 수행 내용 / 결과를 차례로 기술합니다.
      </div>

      {stages.map((st, sIdx) => {
        const s = stageStatus(st.items)
        const { done, total, elapsed } = stageProgress(st.items)
        return (
          <div key={st.id} style={{
            marginBottom: 16, pageBreakInside: 'avoid', breakInside: 'avoid',
          }}>
            <h3 style={{
              fontSize: 14, fontWeight: 800, marginTop: 12, marginBottom: 6,
              padding: '6px 10px',
              background: `${statusColor(s)}15`,
              borderLeft: `4px solid ${statusColor(s)}`,
            }}>
              3.{sIdx + 1} {st.id} · {st.title}
              <span style={{ marginLeft: 8, fontSize: 11, color: statusColor(s) }}>
                {statusIcon(s)} {statusLabel(s)}
              </span>
              <span style={{ marginLeft: 8, fontSize: 10, color: '#6b7280', fontWeight: 500 }}>
                ({done}/{total} · {fmtMs(elapsed)})
              </span>
            </h3>
            <div style={{ fontSize: 10, color: '#4b5563', marginBottom: 6, paddingLeft: 14 }}>
              <b>범위:</b> {st.desc}
            </div>

            {st.items.filter(it => !it.parent).map(it => {
              const isGroup = it.isGroup === true
              const children = isGroup ? st.items.filter(c => c.parent === it.id) : []
              const itStatus = isGroup ? groupStatus(st, it.id) : it.status
              return (
                <div key={it.id} style={{
                  marginBottom: 8, paddingLeft: 14,
                  borderLeft: `2px solid ${statusColor(itStatus)}33`,
                  pageBreakInside: 'avoid', breakInside: 'avoid',
                }}>
                  <div style={{ fontSize: 11, fontWeight: 700, marginBottom: 2 }}>
                    <code style={{ fontSize: 11, color: statusColor(itStatus) }}>{it.id}</code>
                    {' '}— {it.name}
                    {isGroup && <span style={{ color: '#6b7280', fontWeight: 500, fontSize: 10 }}>{' '}(그룹: 자식 {children.length}개)</span>}
                    <span style={{ marginLeft: 8, color: statusColor(itStatus), fontSize: 10 }}>
                      {statusIcon(itStatus)} {statusLabel(itStatus)}
                    </span>
                    {!isGroup && (
                      <span style={{ marginLeft: 6, color: '#6b7280', fontSize: 10 }}>
                        · {fmtMs(it.elapsedMs)}
                      </span>
                    )}
                  </div>
                  {it.desc && (
                    <div style={{ fontSize: 10, color: '#4b5563', marginBottom: 4 }}>
                      <b>설명:</b> {it.desc}
                    </div>
                  )}

                  {/* 자식 항목 list */}
                  {isGroup && children.length > 0 && (
                    <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 10, marginTop: 4 }}>
                      <thead>
                        <tr>
                          {headCell('자식 항목 ID', { width: 240, fontSize: 10, padding: '4px 6px' })}
                          {headCell('이름', { fontSize: 10, padding: '4px 6px' })}
                          {headCell('설명', { fontSize: 10, padding: '4px 6px' })}
                          {headCell('판정', { width: 70, textAlign: 'center', fontSize: 10, padding: '4px 6px' })}
                          {headCell('소요', { width: 50, textAlign: 'right', fontSize: 10, padding: '4px 6px' })}
                        </tr>
                      </thead>
                      <tbody>
                        {children.map(c => (
                          <tr key={c.id}>
                            {cell(c.id.split('.').pop() || c.id, { fontFamily: 'monospace', fontSize: 9, padding: '3px 6px', paddingLeft: 14 })}
                            {cell(c.name, { fontSize: 10, padding: '3px 6px' })}
                            {cell(c.desc || '—', { fontSize: 9, color: '#6b7280', padding: '3px 6px' })}
                            {cell(`${statusIcon(c.status)} ${statusLabel(c.status)}`, {
                              textAlign: 'center', color: statusColor(c.status), fontWeight: 600, fontSize: 9, padding: '3px 6px',
                            })}
                            {cell(fmtMs(c.elapsedMs), { textAlign: 'right', fontSize: 9, color: '#6b7280', padding: '3px 6px' })}
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}
                </div>
              )
            })}
          </div>
        )
      })}

      {/* 푸터 */}
      <div style={{
        marginTop: 24, paddingTop: 12, borderTop: '1px solid #d1d5db',
        fontSize: 9, color: '#9ca3af', textAlign: 'center',
      }}>
        본 보고서는 CIMS 검증 v2 (6단계 파이프라인) 시스템에 의해 자동 생성되었습니다.
        {' · '} 발행 시각: {now}
      </div>
    </div>
  )
}
