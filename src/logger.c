#include <stdio.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include "gcp.h"

/* THREAD 7: Logging & Metrics Thread (prio 40, cpu 2) */
void* logger_thread(void* arg) {
    pin_thread_to_cpu(2);
    set_thread_priority(40);

    int chid = ChannelCreate(0);
    logger_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    struct _pulse pulse;

    while (1) {
        if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) != 0)
            continue;

        if (pulse.code == PULSE_CODE_LOG_EVENT) {
            printf("[logger] latency metric registered\n");
        }
    }
    return NULL;
}
