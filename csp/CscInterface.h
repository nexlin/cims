#ifndef _CSC_INTERFACE_H_
#define _CSC_INTERFACE_H_

#include <string>
#include <thread>

/**
 * @ingroup CspServer
 * @brief Handles TCP Interface with CSC (Config Server)
 *        Receives JSON events for data changes.
 */
class CCscInterface {
public:
    CCscInterface();
    ~CCscInterface();

    bool Start( int iPort );
    void Stop();

private:
    void ListenerLoop();
    void ProcessMessage( const std::string& strMsg, const struct sockaddr_in& clientAddr );

    int m_iPort;
    int m_iServerSock;
    bool m_bRunning;
    std::thread m_threadListener;
};

extern CCscInterface gclsCscInterface;

#endif
