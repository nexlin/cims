#ifndef _SIP_STACK_DEFINE_H_
#define _SIP_STACK_DEFINE_H_

#define USE_TLS


#ifdef USE_TLS

#define USE_TLS_FREE
#endif

#include "SipParserDefine.h"

#define SIP_PACKET_MIN_SIZE		100
#define SIP_PACKET_MAX_SIZE		8192
#define SIP_RING_TIMEOUT			300000

#define SIP_TCP_MAX_SOCKET_PER_THREAD	100
#define SIP_TCP_RECV_TIMEOUT					600
#define SIP_TCP_CONNECT_TIMEOUT				10
#define SIP_TLS_ACCEPT_TIMEOUT				10

#define SIP_UDP_PORT			5060
#define SIP_TCP_PORT			5060
#define SIP_TLS_PORT			5061

#define SIP_STACK_VERSION "1.0"
#define SIP_USER_AGENT	"CIMS_" SIP_STACK_VERSION
#define SIP_MAX_FORWARDS	70

#include "SipMessage.h"

#include <map>

#ifdef USE_HASH_MAP

#include <unordered_map>

#if defined(WIN32) && _MSC_VER == VC2008_VERSION
#define MAP std::tr1::unordered_map
#else
#define MAP std::unordered_map
#endif

#else

#define MAP std::map

#endif

#endif

