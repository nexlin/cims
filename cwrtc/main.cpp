#include "Server.h"
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static volatile bool gbStop = false;

static void SignalHandler(int) { gbStop = true; }

int main(int argc, char* argv[])
{
    const char* pszConfig = argc > 1 ? argv[1] : "cwrtc.json";

    signal(SIGINT,  SignalHandler);
    signal(SIGTERM, SignalHandler);

    if (!StartServer(pszConfig)) {
        fprintf(stderr, "StartServer failed\n");
        return 1;
    }

    printf("cwrtc started. Config: %s  Press Ctrl+C to stop.\n", pszConfig);
    while (!gbStop) sleep(1);

    StopServer();
    return 0;
}
