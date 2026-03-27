#include "mc_video_session.h"

namespace nrtp {

MCVideoSession::MCVideoSession() {
}

MCVideoSession::~MCVideoSession() {
    Close();
}

bool MCVideoSession::Init(const std::string& localIp, uint16_t audioPort, uint16_t videoPort) {
    if (!m_audioSession.Init(localIp, audioPort)) {
        return false;
    }
    
    if (!m_videoSession.Init(localIp, videoPort)) {
        m_audioSession.Close();
        return false;
    }

    return true;
}

void MCVideoSession::SetRemoteAddress(const std::string& ip, uint16_t remoteAudioPort, uint16_t remoteVideoPort) {
    m_audioSession.SetRemoteAddress(ip, remoteAudioPort);
    m_videoSession.SetRemoteAddress(ip, remoteVideoPort);
}

bool MCVideoSession::SendAudio(const uint8_t* data, size_t len, uint32_t timestamp, bool marker) {
    return m_audioSession.SendRtpData(data, len, timestamp, marker);
}

bool MCVideoSession::SendVideo(const uint8_t* data, size_t len, uint32_t timestamp, bool marker) {
    return m_videoSession.SendRtpData(data, len, timestamp, marker);
}

void MCVideoSession::Close() {
    m_audioSession.Close();
    m_videoSession.Close();
}

}
