#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include "mc_video_session.h"
#include "nrtp_rtcp.h"
#include "nrtp_tbcp.h"

// MCVideo Test
void mc_sender_thread() {
    std::cout << "[Sender] Starting MCVideo Session..." << std::endl;
    nrtp::MCVideoSession session;
    
    // Sender binds to 20000 (Audio) and 20002 (Video)
    if (!session.Init("127.0.0.1", 20000, 20002)) {
        std::cerr << "[Sender] Init failed" << std::endl;
        return;
    }
    
    // Remote is 21000/21002
    session.SetRemoteAddress("127.0.0.1", 21000, 21002);

    for (int i = 0; i < 5; ++i) {
        // Audio
        std::string audio = "Audio Frame " + std::to_string(i);
        session.SendAudio((const uint8_t*)audio.data(), audio.size(), i * 160, false);
        std::cout << "[Sender] Sent Audio: " << audio << std::endl;
        
        // Video (send twice per audio to simulate higher rate/frames)
        std::string video = "Video Frame " + std::to_string(i);
        session.SendVideo((const uint8_t*)video.data(), video.size(), i * 3000, true); // Marker=true (Frame End)
        std::cout << "[Sender] Sent Video: " << video << std::endl;
        
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "[Sender] Finished." << std::endl;
}

void mc_receiver_thread() {
    std::cout << "[Receiver] Starting MCVideo Session..." << std::endl;
    nrtp::MCVideoSession session;
    
    // Receiver binds to 21000 (Audio) and 21002 (Video)
    if (!session.Init("127.0.0.1", 21000, 21002)) {
        std::cerr << "[Receiver] Init failed" << std::endl;
        return;
    }
    
    int audio_count = 0;
    int video_count = 0;
    
    auto start = std::chrono::steady_clock::now();
    
    while (audio_count < 5 || video_count < 5) {
        // Poll Audio
        nrtp::NRtpPacket audioPkt;
        if (session.GetAudioSession().ReceiveRtpPacket(audioPkt) > 0) {
             std::string content((char*)audioPkt.GetPayloadData(), audioPkt.GetPayloadSize());
             std::cout << "[Receiver] Got Audio! TS=" << audioPkt.GetTimestamp() << " Content='" << content << "'" << std::endl;
             audio_count++;
        }
        
        // Poll Video
        nrtp::NRtpPacket videoPkt;
        if (session.GetVideoSession().ReceiveRtpPacket(videoPkt) > 0) {
             std::string content((char*)videoPkt.GetPayloadData(), videoPkt.GetPayloadSize());
             std::cout << "[Receiver] Got Video! TS=" << videoPkt.GetTimestamp() << " Content='" << content << "'" << std::endl;
             video_count++;
        }

        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            std::cout << "[Receiver] Timeout." << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "[Receiver] Finished." << std::endl;
}

int main() {
    std::cout << "=== NRTP MCVideo Test ===" << std::endl;
    
    std::thread t_recv(mc_receiver_thread);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); 
    std::thread t_send(mc_sender_thread);
    
    t_send.join();
    t_recv.join();

    std::cout << "=== Test Complete ===" << std::endl;
    return 0;
}
