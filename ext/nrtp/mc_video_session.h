#ifndef MC_VIDEO_SESSION_H
#define MC_VIDEO_SESSION_H

#include <string>
#include "nrtp_session.h"

namespace nrtp {

class MCVideoSession {
public:
    MCVideoSession();
    ~MCVideoSession();

    // Initialize Audio and Video sessions
    bool Init(const std::string& localIp, uint16_t audioPort, uint16_t videoPort);

    // Set Remote Address
    // We assume same IP for both, but ports differ.
    void SetRemoteAddress(const std::string& ip, uint16_t remoteAudioPort, uint16_t remoteVideoPort);

    // Send Methods
    bool SendAudio(const uint8_t* data, size_t len, uint32_t timestamp, bool marker);
    bool SendVideo(const uint8_t* data, size_t len, uint32_t timestamp, bool marker);

    // Accessors
    NRtpSession& GetAudioSession() { return m_audioSession; }
    NRtpSession& GetVideoSession() { return m_videoSession; }

    void Close();

private:
    NRtpSession m_audioSession;
    NRtpSession m_videoSession;
};

}

#endif // MC_VIDEO_SESSION_H
