bool CSipUserAgent::RecvByeRequest( int iThreadId, CSipMessage * pclsMessage )
{
	std::string strCallId;

	if( pclsMessage->GetCallId( strCallId ) == false )
	{
		m_clsSipStack.SendSipMessage( pclsMessage->CreateResponse( SIP_BAD_REQUEST ) );
		return true;
	}

	if( m_pclsCallBack )
	{
		if( m_pclsCallBack->EventIncomingRequestAuth( pclsMessage ) == false )
		{
			return true;
		}
	}

	m_clsSipStack.SendSipMessage( pclsMessage->CreateResponse( SIP_OK ) );

	if( SetCallEnd( strCallId.c_str() ) )
	{
		if( m_pclsCallBack ) m_pclsCallBack->EventCallEnd( strCallId.c_str(), SIP_OK );
		Delete( strCallId.c_str() );
	}

	return true;
}
