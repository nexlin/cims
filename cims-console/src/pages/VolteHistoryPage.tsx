import { useState, useEffect, useCallback } from 'react'
import { callsApi, type CallLog } from '../api/calls'
import { recordingsApi, type RecordingSegment } from '../api/recordings'
import Modal from '../components/Modal'
import FlowPage from './FlowPage'
import SegmentPlayer from '../components/SegmentPlayer'
import { useToast } from '../components/Toast'

function fmtDur(s: number|null) { if(!s||s<=0)return '—'; const m=Math.floor(s/60); return m>0?`${m}분${s%60}초`:`${s}초` }
function fmtTime(iso:string|null) { return iso?iso.replace('T',' ').substring(0,19):'—' }

export default function VolteHistoryPage() {
  const {show}=useToast()
  const [logs,setLogs]=useState<CallLog[]>([])
  const [total,setTotal]=useState(0)
  const [loading,setLoading]=useState(false)
  const [page,setPage]=useState(0)
  const PS=50
  const [fMsisdn,setFM]=useState('')
  const [fDate,setFD]=useState('')
  const [fReason,setFR]=useState('')
  const [autoRefresh,setAR]=useState(false)
  const [detail,setDetail]=useState<CallLog|null>(null)
  const [flow,setFlow]=useState<{callId:string,date:string}|null>(null)
  const [recPlayer,setRecPlayer]=useState<{id:string,segments:RecordingSegment[],callType:'voip'|'ptt'|'voip_video',caller:string,callee:string}|null>(null)

  const load=useCallback(async(p:number)=>{
    setLoading(true)
    try{
      const r=await callsApi.list({call_type:'voip',msisdn:fMsisdn||undefined,date:fDate||undefined,limit:PS,offset:p*PS})
      setLogs(r.logs);setTotal(r.total)
    }catch(e:unknown){show(String(e),'err')}
    finally{setLoading(false)}
  },[show,fMsisdn,fDate])

  useEffect(()=>{setPage(0);load(0)},[load])
  useEffect(()=>{if(!autoRefresh)return;const iv=setInterval(()=>load(page),10000);return()=>clearInterval(iv)},[autoRefresh,load,page])

  const openRecording = async (l: CallLog) => {
    if (!l.dir_name) { show('녹취 디렉터리 정보 없음', 'err'); return }
    try {
      const rec = await recordingsApi.get(l.dir_name)
      if (rec.segments && rec.segments.length > 0) {
        setRecPlayer({ id: l.dir_name, segments: rec.segments, callType: rec.call_type as 'voip'|'voip_video', caller: l.initiator, callee: l.callee })
      } else {
        show('세그먼트 없음', 'err')
      }
    } catch (e: unknown) { show(String(e), 'err') }
  }

  const totalPages=Math.ceil(total/PS)

  return (
    <div className="page">
      <div className="toolbar">
        <input className="search-input" placeholder="발신/착신 번호" value={fMsisdn} onChange={e=>setFM(e.target.value)} onKeyDown={e=>e.key==='Enter'&&(setPage(0),load(0))} style={{maxWidth:180}} />
        <input type="date" className="form-input" value={fDate} onChange={e=>{setFD(e.target.value);setPage(0)}} style={{width:140}} />
        <select className="form-input" value={fReason} onChange={e=>setFR(e.target.value)} style={{width:100}}>
          <option value="">종료사유</option>
          <option value="normal">정상</option><option value="no_answer">무응답</option>
          <option value="busy">통화중</option><option value="rejected">거절</option>
        </select>
        <button className="btn btn--primary btn--sm" onClick={()=>{setPage(0);load(0)}}>검색</button>
        <label style={{marginLeft:'auto',display:'flex',alignItems:'center',gap:4,fontSize:12,color:'var(--text-muted)',cursor:'pointer'}}>
          <input type="checkbox" checked={autoRefresh} onChange={e=>setAR(e.target.checked)}/>자동갱신
        </label>
      </div>

      {loading?<div className="empty">로딩 중...</div>:(
        <div className="table-wrap">
          <table className="data-table">
            <thead><tr>
              <th>유형</th><th>발신</th><th>착신</th><th>상태</th><th>시작시간</th><th>응답시간</th><th>종료시간</th><th>통화시간</th><th>종료사유</th><th style={{width:160}}>작업</th>
            </tr></thead>
            <tbody>
              {logs.length===0?<tr><td colSpan={10} className="empty-cell">이력 없음</td></tr>:
              logs.map(l=>{
                // 통화시간: answer→end (answer 없으면 duration 그대로)
                const dur = l.answer_time && l.end_time
                  ? Math.max(0, Math.floor((new Date(l.end_time).getTime() - new Date(l.answer_time).getTime()) / 1000))
                  : l.duration
                return (
                <tr key={l.id} style={{cursor:'pointer'}} onClick={()=>setDetail(l)}>
                  <td><span className={`badge ${l.call_type==='voip_video'?'badge--blue':'badge--gray'}`} style={{fontSize:10}}>{l.call_type==='voip_video'?'영상':'음성'}</span></td>
                  <td>{l.initiator}</td>
                  <td>{l.callee||'—'}</td>
                  <td><span className={`badge ${l.state==='ended'?'badge--gray':l.state==='active'?'badge--green':l.state==='ringing'?'badge--blue':'badge--gray'}`}>{l.state==='ended'?'종료':l.state==='active'?'통화중':l.state==='ringing'?'호출중':l.state||'—'}</span></td>
                  <td className="ts">{fmtTime(l.invite_time)}</td>
                  <td className="ts">{fmtTime(l.answer_time)}</td>
                  <td className="ts">{fmtTime(l.end_time)}</td>
                  <td className="ts">{fmtDur(dur)}</td>
                  <td className="ts">{l.end_reason_ko||l.end_reason||'—'}</td>
                  <td style={{display:'flex',gap:4,alignItems:'center'}}>
                    <button className="btn btn--sm btn--outline" onClick={e=>{e.stopPropagation();setFlow({callId:l.call_id,date:l.invite_time?.substring(0,10)||''})}}>플로우</button>
                    {l.has_recording && (
                      <button className="btn btn--sm btn--outline" style={{fontSize:10,padding:'1px 6px'}}
                        onClick={e=>{e.stopPropagation();openRecording(l)}}
                        title="녹취 재생">&#9654; 녹취</button>
                    )}
                  </td>
                </tr>
              )})}
            </tbody>
          </table>
        </div>
      )}

      {totalPages>1&&<div className="toolbar" style={{justifyContent:'center',gap:8}}>
        <button className="btn btn--sm btn--outline" disabled={page===0} onClick={()=>{setPage(page-1);load(page-1)}}>← 이전</button>
        <span className="ts">{page+1}/{totalPages} (총 {total}건)</span>
        <button className="btn btn--sm btn--outline" disabled={page>=totalPages-1} onClick={()=>{setPage(page+1);load(page+1)}}>다음 →</button>
      </div>}

      {recPlayer && (
        <div className="modal-overlay" onClick={()=>setRecPlayer(null)}>
          <div className="modal-box" style={{maxWidth:1360, width:'95vw'}} onClick={e=>e.stopPropagation()}>
            <SegmentPlayer
              segments={recPlayer.segments}
              recordingId={recPlayer.id}
              callType={recPlayer.callType}
              caller={recPlayer.caller}
              callee={recPlayer.callee}
              onClose={()=>setRecPlayer(null)}
            />
          </div>
        </div>
      )}

      {flow&&<FlowPage callId={flow.callId} date={flow.date} onClose={()=>setFlow(null)}/>}

      {detail&&(
        <Modal title={`VoLTE 통화 상세`} onClose={()=>setDetail(null)}>
          <dl className="detail-list">
            <dt>발신</dt><dd>{detail.initiator}</dd>
            <dt>착신</dt><dd>{detail.callee}</dd>
            <dt>상태</dt><dd>{detail.state}</dd>
            <dt>시작</dt><dd>{fmtTime(detail.invite_time)}</dd>
            <dt>종료</dt><dd>{fmtTime(detail.end_time)}</dd>
            <dt>통화시간</dt><dd>{fmtDur(detail.duration)}</dd>
            <dt>종료사유</dt><dd>{detail.end_reason_ko||detail.end_reason||'—'}</dd>
          </dl>
          {detail.participants?.length>0&&(
            <>
              <div className="form-section-title" style={{marginTop:12}}>참여자</div>
              <table className="data-table"><thead><tr><th>MSISDN</th><th>역할</th><th>연결</th><th>이탈</th></tr></thead>
              <tbody>{detail.participants.map(p=>(
                <tr key={p.msisdn}><td>{p.msisdn}</td><td className="ts">{p.role==='caller'?'발신':p.role==='callee'?'수신':'멤버'}</td>
                <td className="ts">{fmtTime(p.join_time)}</td><td className="ts">{fmtTime(p.leave_time)}</td></tr>
              ))}</tbody></table>
            </>
          )}
          {detail.has_recording && (
            <div style={{marginTop:12}}>
              <div className="form-section-title">녹취 재생</div>
              <div style={{marginTop:8}}>
                <button className="btn btn--primary btn--sm" onClick={()=>{setDetail(null);openRecording(detail)}}>
                  &#9654; 세그먼트 재생
                </button>
              </div>
            </div>
          )}
          <div className="modal-footer" style={{justifyContent:'space-between'}}>
            <div style={{display:'flex',gap:8}}>
              <button className="btn btn--sm btn--outline" onClick={()=>{setFlow({callId:detail.call_id,date:detail.invite_time?.substring(0,10)||''})}}>플로우 보기</button>
            </div>
            <button className="btn btn--ghost" onClick={()=>setDetail(null)}>닫기</button>
          </div>
        </Modal>
      )}
    </div>
  )
}
