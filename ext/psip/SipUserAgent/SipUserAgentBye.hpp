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
		CSipHeader * pclsReason = pclsMessage->GetHeader( "Reason" );
		if( m_pclsCallBack ) m_pclsCallBack->EventCallEnd( strCallId.c_str(), SIP_OK, pclsReason ? pclsReason->m_strValue.c_str() : NULL );
		Delete( strCallId.c_str() );
	}

	return true;
}
