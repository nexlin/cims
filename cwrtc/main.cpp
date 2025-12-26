#include "H264Decoder.h"
#include "RtpThread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Mock for StartRtpThread if not linked, or used if RtpThread.cpp is compiled.
// Assuming RtpThread.cpp is tailored for the task or we use it.

int main(int argc, char *argv[]) {
  printf("Starting TestWebRtcVideo on Linux...\n");

  // Initialize SSL/DTLS if needed
  // InitDtls(); // Assuming this exists from RtpThread.h

  CRtpThreadArg args;
  args.m_iWebRtcUdpPort = 9000;
  args.m_iPbxUdpPort = 9001;
  args.m_strUserId = "user1";
  args.m_strToId = "user2";

  // In a real scenario, we'd need valid SDP and ICE candidates
  args.m_strSdp = "v=0\r\n...";

  // Start RTP Thread logic
  bool bRes = StartRtpThread(&args);
  if (!bRes) {
    printf("Failed to start RTP thread.\n");
    return -1;
  }

  printf("Running... Press Ctrl+C to exit.\n");
  while (true) {
    usleep(1000000);
  }

  return 0;
}
