import { useState, useEffect, useCallback } from 'react'
import { usersApi, type UserSummary, type Subscription } from '../api/users'
import { orgApi, type Organization } from '../api/organizations'
import { useToast } from '../components/Toast'

// ── 조직+구성원 통합 트리 ─────────────────────────

interface OrgNode { type:'org'; id:number; code:string; name:string; depth:number; memberCount:number; children:TreeItem[] }
interface UserNode { type:'user'; user:UserSummary; depth:number }
type TreeItem = OrgNode | UserNode

function buildTree(orgs: Organization[], users: UserSummary[]): OrgNode[] {
  const map = new Map<number, OrgNode>()
  orgs.forEach(o => map.set(o.id, {type:'org',id:o.id,code:o.code,name:o.name,depth:0,memberCount:0,children:[]}))
  const byOrg = new Map<string, UserSummary[]>()
  users.forEach(u => { const k=u.org_id||''; if(!byOrg.has(k)) byOrg.set(k,[]); byOrg.get(k)!.push(u) })
  const roots: OrgNode[] = []
  orgs.forEach(o => {
    const node = map.get(o.id)!
    if (o.parent_id && map.has(o.parent_id)) map.get(o.parent_id)!.children.push(node)
    else roots.push(node)
  })
  function attach(n:OrgNode, d:number): number {
    n.depth=d; let c=0
    ;(n.children.filter(x=>x.type==='org') as OrgNode[]).forEach(x=>{c+=attach(x,d+1)})
    ;(byOrg.get(n.code)||[]).forEach(u=>{n.children.push({type:'user',user:u,depth:d+1});c++})
    n.memberCount=c; return c
  }
  roots.forEach(r=>attach(r,0))
  const un=byOrg.get('')||[]
  if(un.length){const node:OrgNode={type:'org',id:-1,code:'',name:'미배정',depth:0,memberCount:un.length,children:[]};un.forEach(u=>node.children.push({type:'user',user:u,depth:1}));roots.push(node)}
  return roots
}

function flatTree(nodes:TreeItem[], exp:Set<number>): TreeItem[] {
  const r:TreeItem[]=[]
  function w(list:TreeItem[]){list.forEach(n=>{r.push(n);if(n.type==='org'&&exp.has(n.id))w(n.children)})}
  w(nodes); return r
}

// ── 카드 섹션 컴포넌트 ────────────────────────────

interface CardSectionProps {
  title: string
  svc: 'call' | 'ptt'
  subs: Subscription[]
  userId: number
  onReload: () => void
  hideTitle?: boolean
}

function CardSection({ title, svc, subs, userId, onReload, hideTitle }: CardSectionProps) {
  const { show } = useToast()
  const [editId, setEditId] = useState<string|null>(null)
  const [editForm, setEditForm] = useState<Partial<Subscription>>({})
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<Partial<Subscription>>({id:'',auth_id:'',passwd:'123456',dnd:false,forward_id:''})

  function startEdit(s:Subscription) { setEditId(s.id); setAdding(false); setEditForm({auth_id:s.auth_id,passwd:'',dnd:s.dnd,forward_id:s.forward_id}) }
  async function saveEdit() {
    if(!editId) return
    const d:Partial<Subscription>={...editForm}; if(!d.passwd) delete d.passwd
    try{await usersApi.updateSub(userId,svc,editId,d);show('수정','ok');setEditId(null);onReload()}catch(e:unknown){show(String(e),'err')}
  }
  function startAdd() { setAdding(true); setEditId(null); setAddForm({id:'',auth_id:'',passwd:'123456',dnd:false,forward_id:''}) }
  async function saveAdd() {
    if(!addForm.id){show('MSISDN 필수','err');return}
    try{await usersApi.addSub(userId,svc,addForm);show('추가','ok');setAdding(false);onReload()}catch(e:unknown){show(String(e),'err')}
  }
  async function handleDel(msisdn:string) {
    if(!confirm(`${msisdn} 삭제?`))return
    try{await usersApi.deleteSub(userId,svc,msisdn);show('삭제','ok');onReload()}catch(e:unknown){show(String(e),'err')}
  }

  return (
    <div>
      {!hideTitle && (
        <div style={{fontWeight:600,fontSize:14,marginBottom:8,color:'var(--text-muted)',borderBottom:'1px solid var(--border)',paddingBottom:4}}>
          {title} ({subs.length})
        </div>
      )}
      <div style={{display:'flex',flexDirection:'column',gap:10}}>
        {subs.map(s => {
          const ed = editId===s.id
          return (
            <div key={s.id} className="panel" style={{padding:'12px 16px'}}>
              {ed ? (
                <div style={{display:'grid',gridTemplateColumns:'100px 1fr 100px 1fr',gap:'8px 12px',alignItems:'center',fontSize:13}}>
                  <span style={{fontWeight:600}}>MSISDN</span><span style={{fontWeight:600,gridColumn:'span 3'}}>{s.id}</span>
                  <span>Auth ID</span><input className="form-input" value={editForm.auth_id||''} onChange={e=>setEditForm({...editForm,auth_id:e.target.value})} style={{gridColumn:'span 3'}} />
                  <span>비밀번호</span><input className="form-input" type="password" placeholder="변경 시 입력" value={editForm.passwd||''} onChange={e=>setEditForm({...editForm,passwd:e.target.value})} />
                  <span>DND</span><label style={{display:'flex',alignItems:'center',gap:6}}><input type="checkbox" checked={editForm.dnd||false} onChange={e=>setEditForm({...editForm,dnd:e.target.checked})}/>{editForm.dnd?'ON':'OFF'}</label>
                  <span>착신전환</span><input className="form-input" value={editForm.forward_id||''} onChange={e=>setEditForm({...editForm,forward_id:e.target.value})} style={{gridColumn:'span 3'}} />
                  <div style={{gridColumn:'span 4',display:'flex',gap:8,justifyContent:'flex-end',marginTop:4}}>
                    <button className="btn btn--primary btn--sm" onClick={saveEdit}>저장</button>
                    <button className="btn btn--ghost btn--sm" onClick={()=>setEditId(null)}>취소</button>
                  </div>
                </div>
              ) : (
                <div style={{display:'grid',gridTemplateColumns:'100px 1fr 100px 1fr',gap:'6px 12px',alignItems:'center',fontSize:13}}>
                  <span style={{fontWeight:600}}>MSISDN</span><span style={{fontWeight:700}}>{s.id}</span>
                  <span>DND</span><span className={`badge ${s.dnd?'badge--red':'badge--gray'}`} style={{fontSize:11}}>DND:{s.dnd?'ON':'OFF'}</span>
                  <span>Auth ID</span><span className="ts" style={{gridColumn:'span 3',overflow:'hidden',textOverflow:'ellipsis',whiteSpace:'nowrap'}}>{s.auth_id||'—'}</span>
                  <span>착신전환</span><span className="ts" style={{gridColumn:'span 3'}}>{s.forward_id||'—'}</span>
                  <div style={{gridColumn:'span 4',display:'flex',gap:8,justifyContent:'flex-end',marginTop:2}}>
                    <button className="btn btn--sm btn--outline" onClick={()=>startEdit(s)}>편집</button>
                    <button className="btn btn--sm btn--danger" onClick={()=>handleDel(s.id)}>삭제</button>
                  </div>
                </div>
              )}
            </div>
          )
        })}

        {adding ? (
          <div className="panel" style={{padding:'12px 16px',borderStyle:'dashed'}}>
            <div style={{display:'grid',gridTemplateColumns:'100px 1fr 100px 1fr',gap:'8px 12px',alignItems:'center',fontSize:13}}>
              <span>MSISDN *</span><input className="form-input" placeholder={svc==='call'?'+821357007xxx':'+82571900xxx'} value={addForm.id||''} onChange={e=>setAddForm({...addForm,id:e.target.value})} autoFocus style={{gridColumn:'span 3'}} />
              <span>Auth ID</span><input className="form-input" placeholder="미입력 시 자동" value={addForm.auth_id||''} onChange={e=>setAddForm({...addForm,auth_id:e.target.value})} style={{gridColumn:'span 3'}} />
              <span>비밀번호</span><input className="form-input" value={addForm.passwd||''} onChange={e=>setAddForm({...addForm,passwd:e.target.value})} />
              <span>DND</span><label style={{display:'flex',alignItems:'center',gap:6}}><input type="checkbox" checked={addForm.dnd||false} onChange={e=>setAddForm({...addForm,dnd:e.target.checked})}/>{addForm.dnd?'ON':'OFF'}</label>
              <span>착신전환</span><input className="form-input" placeholder="번호" value={addForm.forward_id||''} onChange={e=>setAddForm({...addForm,forward_id:e.target.value})} style={{gridColumn:'span 3'}} />
              <div style={{gridColumn:'span 4',display:'flex',gap:8,justifyContent:'flex-end',marginTop:4}}>
                <button className="btn btn--primary btn--sm" onClick={saveAdd}>저장</button>
                <button className="btn btn--ghost btn--sm" onClick={()=>setAdding(false)}>취소</button>
              </div>
            </div>
          </div>
        ) : (
          <div className="panel" style={{padding:12,borderStyle:'dashed',textAlign:'center',cursor:'pointer',color:'var(--primary)',fontSize:13}} onClick={startAdd}>
            ＋ {title.replace(' 번호','')} 번호 추가
          </div>
        )}
      </div>
    </div>
  )
}

// ── 메인 ────────────────────────────────────────────

export default function SubscriptionsPage() {
  const { show } = useToast()
  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [loading, setLoading] = useState(true)
  const [expanded, setExpanded] = useState<Set<number>>(new Set())
  const [selectedUser, setSelectedUser] = useState<UserSummary | null>(null)

  const load = useCallback(async () => {
    setLoading(true)
    try { const [u,o]=await Promise.all([usersApi.list(),orgApi.list()]); setUsers(u); setOrgs(o); setExpanded(new Set(o.map(x=>x.id))) }
    catch(e:unknown){show(String(e),'err')}
    finally{setLoading(false)}
  }, [show])
  useEffect(()=>{load()},[load])

  useEffect(()=>{
    if(selectedUser){ const up=users.find(u=>u.id===selectedUser.id); if(up)setSelectedUser(up) }
  },[users])

  const tree = buildTree(orgs, users)
  const flat = flatTree(tree, expanded)

  function toggleExp(id:number){setExpanded(p=>{const n=new Set(p);if(n.has(id))n.delete(id);else n.add(id);return n})}

  return (
    <div style={{display:'flex',gap:12,alignItems:'stretch',height:'calc(100vh - 100px)'}}>
      {/* 좌측 트리: 좁게, 상하 최대 */}
      <div className="panel" style={{width:190,minWidth:190,display:'flex',flexDirection:'column',overflow:'hidden'}}>
        <div style={{padding:'8px 10px',fontWeight:600,fontSize:12,borderBottom:'1px solid var(--border)',flexShrink:0}}>조직 / 구성원</div>
        {loading?<div className="empty" style={{padding:12,fontSize:12}}>로딩 중...</div>:(
          <div style={{flex:1,overflowY:'auto'}}>
            {flat.map(n=>n.type==='org'?(
              <div key={`o${n.id}`} style={{display:'flex',alignItems:'center',gap:3,paddingLeft:6+n.depth*14,paddingRight:6,paddingTop:4,paddingBottom:4,cursor:'pointer',fontSize:11}}
                onClick={()=>toggleExp(n.id)}>
                <span style={{width:12,textAlign:'center',fontSize:9,userSelect:'none'}}>{n.children.length>0?(expanded.has(n.id)?'▼':'▶'):'●'}</span>
                <span style={{fontWeight:600,flex:1,overflow:'hidden',textOverflow:'ellipsis',whiteSpace:'nowrap'}}>{n.name}</span>
                {n.memberCount>0&&<span className="badge badge--blue" style={{fontSize:9,padding:'0 4px',lineHeight:'16px'}}>{n.memberCount}</span>}
              </div>
            ):(
              <div key={`u${n.user.id}`} style={{display:'flex',alignItems:'center',gap:3,paddingLeft:6+n.depth*14,paddingRight:6,paddingTop:3,paddingBottom:3,
                cursor:'pointer',fontSize:11,background:selectedUser?.id===n.user.id?'rgba(74,144,217,0.15)':undefined}}
                onClick={()=>{setSelectedUser(n.user)}}>
                <span style={{width:12}}></span>
                <span style={{fontWeight:selectedUser?.id===n.user.id?600:400,color:selectedUser?.id===n.user.id?'var(--primary)':undefined}}>{n.user.name}</span>
              </div>
            ))}
          </div>
        )}
      </div>

      {/* 우측: VoLTE/PTT 상하 반분할 */}
      <div style={{flex:1,display:'flex',flexDirection:'column',overflow:'hidden'}}>
        {!selectedUser?(
          <div className="empty" style={{flex:1,display:'flex',alignItems:'center',justifyContent:'center'}}>좌측에서 구성원을 선택하세요</div>
        ):(
          <>
            {/* 사용자 헤더 */}
            <div style={{marginBottom:8,display:'flex',alignItems:'center',gap:10,flexShrink:0}}>
              <span style={{fontWeight:700,fontSize:15}}>{selectedUser.name}</span>
              <span className="ts">{selectedUser.org_id||''}</span>
              {selectedUser.login_id&&<span className="ts">({selectedUser.login_id})</span>}
            </div>

            {/* VoLTE 영역: 상반 */}
            <div style={{flex:1,minHeight:0,display:'flex',flexDirection:'column',marginBottom:8}}>
              <div style={{fontWeight:600,fontSize:13,color:'var(--text-muted)',borderBottom:'1px solid var(--border)',paddingBottom:4,marginBottom:8,flexShrink:0}}>
                VoLTE 번호 ({selectedUser.call_subscriptions.length})
              </div>
              <div style={{flex:1,overflowY:'auto'}}>
                <CardSection title="VoLTE 번호" svc="call" subs={selectedUser.call_subscriptions} userId={selectedUser.id} onReload={load} hideTitle />
              </div>
            </div>

            {/* PTT 영역: 하반 */}
            <div style={{flex:1,minHeight:0,display:'flex',flexDirection:'column'}}>
              <div style={{fontWeight:600,fontSize:13,color:'var(--text-muted)',borderBottom:'1px solid var(--border)',paddingBottom:4,marginBottom:8,flexShrink:0}}>
                PTT 번호 ({selectedUser.ptt_subscriptions.length})
              </div>
              <div style={{flex:1,overflowY:'auto'}}>
                <CardSection title="PTT 번호" svc="ptt" subs={selectedUser.ptt_subscriptions} userId={selectedUser.id} onReload={load} hideTitle />
              </div>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
