#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include "gcp.h"

/* THREAD 5: GCP Internal Watchdog Thread (prio 58, cpu 2) */
void* internal_watchdog_thread(void* arg) {
    pin_thread_to_cpu(2);
    set_thread_priority(58);

    /* Spin until the decision engine has published its connection ID */
    while (de_coid == -1)
        delay(10);

    int chid = ChannelCreate(0);
    struct sigevent  event;
    timer_t          timer_id;
    struct itimerspec timer_spec;
    int strikes = 0;

    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    SIGEV_PULSE_INIT(&event, coid, SIGEV_PULSE_PRIO_INHERIT,
                     PULSE_CODE_WD_KICK, 0);
    timer_create(CLOCK_REALTIME, &event, &timer_id);

    timer_spec.it_value.tv_sec    = 1;
    timer_spec.it_value.tv_nsec   = 0;
    timer_spec.it_interval.tv_sec = 1;
    timer_spec.it_interval.tv_nsec= 0;
    timer_settime(timer_id, 0, &timer_spec, NULL);

    struct _pulse pulse;
    while (1) {
        if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) != 0)
            continue;

        if (de_alive_flag == 1) {
            strikes       = 0;
            de_alive_flag = 0;
        } else {
            strikes++;
            printf("[gcp self-watch] strike %d! decision loop stuck\n", strikes);
            if (strikes >= 3) {
                printf("[gcp self-watch] fatal internal error! crashing...\n");
                exit(EXIT_FAILURE);
            }
        }

        /* Poke the DE — it must respond with a real work pulse before the
           next tick. WD_KICK is filtered out in the DE so it does NOT
           reset de_alive_flag, ensuring true liveness detection.         */
        int de = de_coid;
        MsgSendPulse(de, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE_WD_KICK, 0);
    }
    return NULL;
}
