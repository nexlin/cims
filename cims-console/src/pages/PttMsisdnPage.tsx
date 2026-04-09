import { useState, useEffect, useCallback } from 'react'
import { usersApi, type UserSummary, type Subscription } from '../api/users'
import { orgApi, type Organization } from '../api/organizations'
import { useToast } from '../components/Toast'

// ── 조직+구성원 통합 트리 노드 ─────────────────────

interface OrgNode { type: 'org'; id: number; code: string; codePath: string; name: string; depth: number; memberCount: number; children: TreeItem[] }
interface UserNode { type: 'user'; user: UserSummary; depth: number }
type TreeItem = OrgNode | UserNode

function buildTree(orgs: Organization[], users: UserSummary[]): OrgNode[] {
  const map = new Map<number, OrgNode>()
  orgs.forEach(o => map.set(o.id, { type:'org', id:o.id, code:o.code, codePath:o.code_path||o.code, name:o.name, depth:0, memberCount:0, children:[] }))
  const byOrg = new Map<string, UserSummary[]>()
  users.forEach(u => { const k = u.org_id||''; if(!byOrg.has(k)) byOrg.set(k,[]); byOrg.get(k)!.push(u) })
  const roots: OrgNode[] = []
  map.forEach(n => { if(n.id > 0 && map.has(orgs.find(o=>o.id===n.id)?.parent_id!)) map.get(orgs.find(o=>o.id===n.id)!.parent_id!)!.children.push(n); else roots.push(n) })
  function attach(n: OrgNode, d: number): number {
    n.depth = d; let c = 0
    const childOrgs = n.children.filter(x=>x.type==='org') as OrgNode[]
    childOrgs.forEach(x => { c += attach(x, d+1) })
    ;(byOrg.get(n.code)||[]).forEach(u => { n.children.push({type:'user',user:u,depth:d+1}); c++ })
    n.memberCount = c; return c
  }
  roots.forEach(r => attach(r, 0))
  const un = byOrg.get('')||[]
  if(un.length) { const node: OrgNode = {type:'org',id:-1,code:'',codePath:'',name:'미배정',depth:0,memberCount:un.length,children:[]}; un.forEach(u=>node.children.push({type:'user',user:u,depth:1})); roots.push(node) }
  return roots
}

function flatten(nodes: TreeItem[], exp: Set<number>): TreeItem[] {
  const r: TreeItem[] = []
  function w(list: TreeItem[]) { list.forEach(n => { r.push(n); if(n.type==='org'&&exp.has(n.id)) w(n.children) }) }
  w(nodes); return r
}

// ── 메인 ────────────────────────────────────────────

export default function PttMsisdnPage() {
  const { show } = useToast()
  const [users, setUsers] = useState<UserSummary[]>([])
  const [orgs, setOrgs] = useState<Organization[]>([])
  const [loading, setLoading] = useState(true)
  const [expanded, setExpanded] = useState<Set<number>>(new Set())
  const [selectedUser, setSelectedUser] = useState<UserSummary | null>(null)
  const [editMsisdn, setEditMsisdn] = useState<string | null>(null)
  const [editForm, setEditForm] = useState<Partial<Subscription>>({})
  const [adding, setAdding] = useState(false)
  const [addForm, setAddForm] = useState<Partial<Subscription>>({ id:'', auth_id:'', passwd:'123456', dnd:false, forward_id:'' })

  const load = useCallback(async () => {
    setLoading(true)
    try { const [u,o] = await Promise.all([usersApi.list(), orgApi.list()]); setUsers(u); setOrgs(o); setExpanded(new Set(o.map(x=>x.id))) }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [show])
  useEffect(() => { load() }, [load])

  // 선택된 사용자가 reload 후에도 유지되도록
  useEffect(() => {
    if (selectedUser) {
      const updated = users.find(u => u.id === selectedUser.id)
      if (updated) setSelectedUser(updated)
    }
  }, [users])

  const tree = buildTree(orgs, users)
  const flat = flatten(tree, expanded)
  const subs = selectedUser?.ptt_subscriptions || []

  function toggleExp(id: number) { setExpanded(p => { const n=new Set(p); if(n.has(id)) n.delete(id); else n.add(id); return n }) }
  function select(u: UserSummary) { setSelectedUser(u); setEditMsisdn(null); setAdding(false) }

  function startEdit(s: Subscription) { setEditMsisdn(s.id); setAdding(false); setEditForm({auth_id:s.auth_id,passwd:'',dnd:s.dnd,forward_id:s.forward_id}) }
  async function saveEdit() {
    if(!selectedUser||!editMsisdn) return
    const d: Partial<Subscription> = {...editForm}; if(!d.passwd) delete d.passwd
    try { await usersApi.updateSub(selectedUser.id,'ptt',editMsisdn,d); show('수정','ok'); setEditMsisdn(null); load() } catch(e:unknown){show(String(e),'err')}
  }
  function startAdd() { setAdding(true); setEditMsisdn(null); setAddForm({id:'',auth_id:'',passwd:'123456',dnd:false,forward_id:''}) }
  async function saveAdd() {
    if(!selectedUser||!addForm.id){show('MSISDN 필수','err');return}
    try { await usersApi.addSub(selectedUser.id,'ptt',addForm); show('추가','ok'); setAdding(false); load() } catch(e:unknown){show(String(e),'err')}
  }
  async function handleDel(msisdn:string) {
    if(!selectedUser||!confirm(`${msisdn} 삭제?`)) return
    try { await usersApi.deleteSub(selectedUser.id,'ptt',msisdn); show('삭제','ok'); load() } catch(e:unknown){show(String(e),'err')}
  }

  return (
    <div style={{ display:'flex', gap:16, alignItems:'flex-start' }}>
      {/* 좌측: 조직+구성원 트리 */}
      <div className="panel" style={{ width:260, minHeight:400 }}>
        <div style={{ padding:'10px 12px', fontWeight:600, fontSize:13, borderBottom:'1px solid var(--border)' }}>조직 / 구성원</div>
        {loading ? <div className="empty" style={{padding:12}}>로딩 중...</div> : (
          <div style={{ maxHeight:500, overflowY:'auto' }}>
            {flat.map((n) => n.type==='org' ? (
              <div key={`o${n.id}`} style={{ display:'flex', alignItems:'center', gap:4, paddingLeft:8+n.depth*16, paddingRight:8, paddingTop:5, paddingBottom:5, cursor:'pointer', fontSize:12 }}
                onClick={()=>toggleExp(n.id)}>
                <span style={{width:14,textAlign:'center',fontSize:10,userSelect:'none'}}>{n.children.length>0?(expanded.has(n.id)?'▼':'▶'):'●'}</span>
                <span style={{fontWeight:600,flex:1}}>{n.name}</span>
                {n.memberCount>0&&<span className="badge badge--blue" style={{fontSize:10,padding:'1px 6px'}}>{n.memberCount}</span>}
              </div>
            ) : (
              <div key={`u${n.user.id}`} style={{ display:'flex', alignItems:'center', gap:4, paddingLeft:8+n.depth*16, paddingRight:8, paddingTop:4, paddingBottom:4,
                cursor:'pointer', fontSize:12, background:selectedUser?.id===n.user.id?'rgba(74,144,217,0.15)':undefined }}
                onClick={()=>select(n.user)}>
                <span style={{width:14}}></span>
                <span style={{fontWeight:selectedUser?.id===n.user.id?600:400, color:selectedUser?.id===n.user.id?'var(--primary)':undefined}}>{n.user.name}</span>
              </div>
            ))}
          </div>
        )}
      </div>

      {/* 우측: MSISDN 카드 (가로 넓은 카드, 세로 쌓기) */}
      <div style={{ flex:1 }}>
        {!selectedUser ? (
          <div className="empty" style={{padding:40}}>좌측에서 구성원을 선택하세요</div>
        ) : (
          <>
            <div style={{ marginBottom:16, fontWeight:600, fontSize:15 }}>{selectedUser.name}의 PTT 번호</div>
            <div style={{ display:'flex', flexDirection:'column', gap:12 }}>
              {subs.map(s => {
                const ed = editMsisdn===s.id
                return (
                  <div key={s.id} className="panel" style={{ padding:16 }}>
                    {ed ? (
                      <div style={{ display:'grid', gridTemplateColumns:'100px 1fr 100px 1fr', gap:'8px 12px', alignItems:'center', fontSize:13 }}>
                        <span style={{fontWeight:600}}>MSISDN</span><span style={{fontWeight:600,gridColumn:'span 3'}}>{s.id}</span>
                        <span>Auth ID</span><input className="form-input" value={editForm.auth_id||''} onChange={e=>setEditForm({...editForm,auth_id:e.target.value})} style={{gridColumn:'span 3'}} />
                        <span>비밀번호</span><input className="form-input" type="password" placeholder="변경 시 입력" value={editForm.passwd||''} onChange={e=>setEditForm({...editForm,passwd:e.target.value})} />
                        <span>DND</span><label style={{display:'flex',alignItems:'center',gap:6}}><input type="checkbox" checked={editForm.dnd||false} onChange={e=>setEditForm({...editForm,dnd:e.target.checked})} />{editForm.dnd?'ON':'OFF'}</label>
                        <span>착신전환</span><input className="form-input" value={editForm.forward_id||''} onChange={e=>setEditForm({...editForm,forward_id:e.target.value})} style={{gridColumn:'span 3'}} />
                        <div style={{gridColumn:'span 4',display:'flex',gap:8,justifyContent:'flex-end',marginTop:4}}>
                          <button className="btn btn--primary btn--sm" onClick={saveEdit}>저장</button>
                          <button className="btn btn--ghost btn--sm" onClick={()=>setEditMsisdn(null)}>취소</button>
                        </div>
                      </div>
                    ) : (
                      <div style={{ display:'flex', alignItems:'center', gap:16, fontSize:13 }}>
                        <span style={{fontWeight:700,fontSize:14,minWidth:160}}>{s.id}</span>
                        <span className="ts" style={{flex:1}}>Auth: {s.auth_id?.substring(0,30)}{(s.auth_id?.length||0)>30?'...':''}</span>
                        <span className={`badge ${s.dnd?'badge--red':'badge--gray'}`} style={{fontSize:11}}>DND:{s.dnd?'ON':'OFF'}</span>
                        {s.forward_id&&<span className="ts">→{s.forward_id}</span>}
                        <button className="btn btn--sm btn--outline" onClick={()=>startEdit(s)}>편집</button>
                        <button className="btn btn--sm btn--danger" onClick={()=>handleDel(s.id)}>삭제</button>
                      </div>
                    )}
                  </div>
                )
              })}

              {adding ? (
                <div className="panel" style={{ padding:16, borderStyle:'dashed' }}>
                  <div style={{ display:'grid', gridTemplateColumns:'100px 1fr 100px 1fr', gap:'8px 12px', alignItems:'center', fontSize:13 }}>
                    <span>MSISDN *</span><input className="form-input" placeholder="+821357007xxx" value={addForm.id||''} onChange={e=>setAddForm({...addForm,id:e.target.value})} autoFocus style={{gridColumn:'span 3'}} />
                    <span>Auth ID</span><input className="form-input" placeholder="미입력 시 자동" value={addForm.auth_id||''} onChange={e=>setAddForm({...addForm,auth_id:e.target.value})} style={{gridColumn:'span 3'}} />
                    <span>비밀번호</span><input className="form-input" value={addForm.passwd||''} onChange={e=>setAddForm({...addForm,passwd:e.target.value})} />
                    <span>DND</span><label style={{display:'flex',alignItems:'center',gap:6}}><input type="checkbox" checked={addForm.dnd||false} onChange={e=>setAddForm({...addForm,dnd:e.target.checked})} />{addForm.dnd?'ON':'OFF'}</label>
                    <span>착신전환</span><input className="form-input" placeholder="번호" value={addForm.forward_id||''} onChange={e=>setAddForm({...addForm,forward_id:e.target.value})} style={{gridColumn:'span 3'}} />
                    <div style={{gridColumn:'span 4',display:'flex',gap:8,justifyContent:'flex-end',marginTop:4}}>
                      <button className="btn btn--primary btn--sm" onClick={saveAdd}>저장</button>
                      <button className="btn btn--ghost btn--sm" onClick={()=>setAdding(false)}>취소</button>
                    </div>
                  </div>
                </div>
              ) : (
                <div className="panel" style={{ padding:16, borderStyle:'dashed', textAlign:'center', cursor:'pointer', color:'var(--primary)' }} onClick={startAdd}>
                  ＋ PTT 번호 추가
                </div>
              )}
            </div>
          </>
        )}
      </div>
    </div>
  )
}
