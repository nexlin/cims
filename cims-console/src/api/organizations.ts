import { api } from './client'

export interface Organization {
  id: number
  code: string
  code_path: string
  name: string
  parent_id: number | null
  sort_order: number
}

export interface OrgInput {
  code: string
  name: string
  parent_id?: number | null
  sort_order?: number
}

export const orgApi = {
  list:         ()                            => api.get<{organizations: Organization[]}>('/organizations').then(r => r.organizations),
  get:          (id: number)                  => api.get<Organization>(`/organizations/${id}`),
  create:       (data: OrgInput)              => api.post<{id:number, code:string}>('/organizations', data),
  update:       (id: number, data: Partial<OrgInput>) => api.put<{id:number}>(`/organizations/${id}`, data),
  delete:       (id: number)                  => api.delete<{id:number}>(`/organizations/${id}`),
  batchDelete:  (ids: number[])               => api.delete<{deleted:number}>('/organizations/batch', {ids}),
  users:        (id: number)                  => api.get<{users: Array<{id:number,name:string,login_id:string,org_id:string}>}>(`/organizations/${id}/users`).then(r => r.users),
  importExcel:  (base64: string)              => api.post<{created:number, updated:number, errors:Array<{row:number,error:string}>}>('/organizations/import', {file_base64: base64}),
  templateUrl:  '/api/v1/organizations/import/template',
}
