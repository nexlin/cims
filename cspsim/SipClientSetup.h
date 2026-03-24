#ifndef _SIP_CLIENT_SETUP_H_
#define _SIP_CLIENT_SETUP_H_

#include <string>
#include "SipTransport.h"

class CSipClientSetup
{
public:
	CSipClientSetup();
	~CSipClientSetup();

	int					m_iUdpPort;

	std::string	m_strLocalIp;

	std::string	m_strSipServerIp;

	int					m_iSipServerPort;

	std::string	m_strSipDomain;

	std::string	m_strSipUserId;

	std::string	m_strSipPassWord;

	ESipTransport	m_eSipTransport;

	std::string		m_strPemFile;

	std::string		m_strSpeaker;

	std::string		m_strMic;

	bool Read( const char * pszFileName );
};

extern CSipClientSetup gclsSetupFile;

#endif
