/* 
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com> (http://blog.naver.com/websearch)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */

// SIP 로그인 정보를 추가한다.
bool CSipUserAgent::InsertRegisterInfo( CSipServerInfo & clsInfo )
{
	if( clsInfo.m_strIp.empty() ) return false;
	if( clsInfo.m_strUserId.empty() ) return false;

	if( clsInfo.m_strDomain.empty() )
	{
		clsInfo.m_strDomain = clsInfo.m_strIp;
	}

	SIP_SERVER_INFO_LIST::iterator	it;
	bool	bFound = false;

	m_clsRegisterMutex.acquire();
	for( it = m_clsRegisterList.begin(); it != m_clsRegisterList.end(); ++it )
	{
		if( it->Equal( clsInfo ) )
		{
			bFound = true;
			break;
		}
	}

	if( bFound == false )
	{
		m_clsRegisterList.push_back( clsInfo );
	}
	m_clsRegisterMutex.release();

	return true;
}

// SIP 로그인 정보를 수정한다.
bool CSipUserAgent::UpdateRegisterInfo( CSipServerInfo & clsInfo )
{
	if( clsInfo.m_strIp.empty() ) return false;
	if( clsInfo.m_strUserId.empty() ) return false;

	SIP_SERVER_INFO_LIST::iterator	it;
	bool	bRes = false;

	m_clsRegisterMutex.acquire();
	for( it = m_clsRegisterList.begin(); it != m_clsRegisterList.end(); ++it )
	{
		if( it->Equal( clsInfo ) )
		{
			bRes = true;
			it->Update( clsInfo );
			break;
		}
	}
	m_clsRegisterMutex.release();

	return bRes;
}

// SIP 로그인 정보를 삭제한다.
bool CSipUserAgent::DeleteRegisterInfo( CSipServerInfo & clsInfo )
{
	if( clsInfo.m_strIp.empty() ) return false;
	if( clsInfo.m_strUserId.empty() ) return false;

	SIP_SERVER_INFO_LIST::iterator	it;
	bool	bRes = false;

	m_clsRegisterMutex.acquire();
	for( it = m_clsRegisterList.begin(); it != m_clsRegisterList.end(); ++it )
	{
		if( it->Equal( clsInfo ) )
		{
			bRes = true;

			if( it->m_iLoginTimeout == 0 )
			{
				m_clsRegisterList.erase( it );
			}
			else
			{
				it->m_bDelete = true;
				it->m_iLoginTimeout = 0;
			}

			break;
		}
	}
	m_clsRegisterMutex.release();

	return bRes;
}

// 모든 SIP 로그인 정보를 삭제한다.
void CSipUserAgent::DeleteRegisterInfoAll( )
{
	m_clsRegisterMutex.acquire();
	m_clsRegisterList.clear();
	m_clsRegisterMutex.release();
}

// 로그아웃한다.
void CSipUserAgent::DeRegister( )
{
	SIP_SERVER_INFO_LIST::iterator	it;

	m_clsRegisterMutex.acquire();
	for( it = m_clsRegisterList.begin(); it != m_clsRegisterList.end(); ++it )
	{
		it->m_bDelete = true;
		it->m_iLoginTimeout = 0;
	}
	m_clsRegisterMutex.release();
}
