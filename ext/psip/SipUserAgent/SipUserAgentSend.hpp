/*
 * Copyright (C) 2012 Yee Young Han <websearch@naver.com>
 * (http://blog.naver.com/websearch)
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

/**
 * @ingroup SipUserAgent
 * @brief ReINVITE 메시지를 전송한다.
 * @param pszCallId SIP Call-ID
 * @param pclsRtp		local RTP 정보 저장 객체
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CSipUserAgent::SendReInvite(const char *pszCallId, CSipCallRtp *pclsRtp) {
  SIP_DIALOG_MAP::iterator itMap;
  CSipMessage *pclsRequest = NULL;
  bool bRes = false;

  m_clsDialogMutex.acquire();
  itMap = m_clsDialogMap.find(pszCallId);
  if (itMap != m_clsDialogMap.end()) {
    itMap->second.SetLocalRtp(pclsRtp);
    pclsRequest = itMap->second.CreateInvite();
    bRes = true;
  }
  m_clsDialogMutex.release();

  if (pclsRequest) {
    m_clsSipStack.SendSipMessage(pclsRequest);
  }

  return bRes;
}

/**
 * @ingroup SipUserAgent
 * @brief Blind Transfer 에서 사용되는 NOTIFY 메시지를 전송한다.
 * @param pszCallId SIP Call-ID
 * @param iSipCode	INVITE 응답 메시지의 SIP status code
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CSipUserAgent::SendNotify(const char *pszCallId, int iSipCode) {
  SIP_DIALOG_MAP::iterator itMap;
  CSipMessage *pclsRequest = NULL;
  bool bRes = false;

  m_clsDialogMutex.acquire();
  itMap = m_clsDialogMap.find(pszCallId);
  if (itMap != m_clsDialogMap.end()) {
    pclsRequest = itMap->second.CreateNotify();
    bRes = true;
  }
  m_clsDialogMutex.release();

  if (pclsRequest) {
    char szBuf[255];

    pclsRequest->m_clsContentType.Set("message", "sipfrag");
    pclsRequest->m_clsContentType.InsertParam("version", "2.0");
    pclsRequest->AddHeader("Event", "refer");

    if (iSipCode >= 200) {
      pclsRequest->AddHeader("Subscription-State", "terminated");
    } else {
      pclsRequest->AddHeader("Subscription-State", "active");
    }

    pclsRequest->m_iContentLength =
        snprintf(szBuf, sizeof(szBuf), "SIP/2.0 %d %s", iSipCode,
                 SipGetReasonPhrase(iSipCode));
    pclsRequest->m_strBody = szBuf;

    m_clsSipStack.SendSipMessage(pclsRequest);
  }

  return bRes;
}

/**
 * @ingroup SipUserAgent
 * @brief 특정 Call-ID dialog에 in-dialog NOTIFY를 전송한다 (범용).
 */
bool CSipUserAgent::SendNotifyWithBody(const char *pszCallId, const char *pszEvent,
                                        const char *pszContentType, const char *pszContentSubType,
                                        const std::string &strBody) {
  SIP_DIALOG_MAP::iterator itMap;
  CSipMessage *pclsRequest = NULL;

  m_clsDialogMutex.acquire();
  itMap = m_clsDialogMap.find(pszCallId);
  if (itMap != m_clsDialogMap.end()) {
    pclsRequest = itMap->second.CreateNotify();
  }
  m_clsDialogMutex.release();

  if (pclsRequest) {
    pclsRequest->m_clsContentType.Set(pszContentType, pszContentSubType);
    pclsRequest->AddHeader("Event", pszEvent);
    pclsRequest->AddHeader("Subscription-State", "active");
    pclsRequest->m_strBody = strBody;
    pclsRequest->m_iContentLength = (int)strBody.size();
    m_clsSipStack.SendSipMessage(pclsRequest);
    return true;
  }
  return false;
}

/**
 * @ingroup SipUserAgent
 * @brief INFO 메시지로 DTMF 를 전송한다.
 * @param pszCallId SIP Call-ID
 * @param cDtmf			DTMF 문자. '0' ~ '9' 및 '*', '#'
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CSipUserAgent::SendDtmf(const char *pszCallId, char cDtmf) {
  SIP_DIALOG_MAP::iterator itMap;
  CSipMessage *pclsRequest = NULL;
  bool bRes = false;

  m_clsDialogMutex.acquire();
  itMap = m_clsDialogMap.find(pszCallId);
  if (itMap != m_clsDialogMap.end()) {
    pclsRequest = itMap->second.CreateInfo();
    bRes = true;
  }
  m_clsDialogMutex.release();

  if (pclsRequest) {
    char szBody[51];

    pclsRequest->m_iContentLength =
        snprintf(szBody, sizeof(szBody), "Signal=%c\r\nDuration=160", cDtmf);
    pclsRequest->m_strBody = szBody;

    pclsRequest->m_clsContentType.Set("application", "dtmf-relay");

    m_clsSipStack.SendSipMessage(pclsRequest);
  }

  return bRes;
}

/**
 * @ingroup SipUserAgent
 * @brief SIP PRACK 메시지를 전송한다.
 * @param pszCallId SIP Call-ID
 * @param pclsRtp		local RTP 정보 저장 객체
 * @returns 성공하면 true 를 리턴하고 실패하면 false 를 리턴한다.
 */
bool CSipUserAgent::SendPrack(const char *pszCallId, CSipCallRtp *pclsRtp) {
  SIP_DIALOG_MAP::iterator itMap;
  bool bRes = false;
  CSipMessage *pclsMessage = NULL;

  m_clsDialogMutex.acquire();
  itMap = m_clsDialogMap.find(pszCallId);
  if (itMap != m_clsDialogMap.end()) {
    // 통화 연결되지 않고 발신한 경우에만 PRACK 메시지를 생성한다.
    if (itMap->second.m_sttStartTime.tv_sec == 0 &&
        itMap->second.m_pclsInvite == NULL) {
      pclsMessage = itMap->second.CreatePrack();
      if (pclsMessage) {
        itMap->second.SetLocalRtp(pclsRtp);

        if (pclsRtp) {
          itMap->second.AddSdp(pclsMessage);
        }

        bRes = true;
      }
    }
  }
  m_clsDialogMutex.release();

  if (pclsMessage) {
    m_clsSipStack.SendSipMessage(pclsMessage);
  }

  return bRes;
}
