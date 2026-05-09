#include <stdio.h>
#include <sys/neutrino.h>
#include <pthread.h>
#include "gcp.h"

/* THREAD 3: Priority Queue Manager Thread (prio 61, cpu 2) */
void* priority_queue_thread(void* arg) {
    // pin to core 2 to offload dispatch thread on core 1
    pin_thread_to_cpu(2);
    // run at 61 so we process queue stuff right under dispatch priority
    set_thread_priority(61);

    // create qnx channel to receive overload triggers
    int chid = ChannelCreate(0);
    // publish connection id globally so dispatch thread can ping us
    q_manager_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    struct _pulse pulse;

    while (1) {
        // block until we get a pulse, skip if call fails
        if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) != 0)
            continue;

        // extract which zone is overloaded from the incoming pulse data
        int overloaded_zone = pulse.value.sival_int;

        // grab mutex lock before touching the shared queue array
        pthread_mutex_lock(&state_mutex);

        /* Slot 0 is highest priority. Hospital (id 0) beats industrial (id 1).
           Only two zones can ever be overloaded simultaneously. */
        // if queue is totally empty, stick it in the front slot
        if (priority_queue[0] == -1) {
            priority_queue[0] = overloaded_zone;
        } else if (priority_queue[0] != overloaded_zone &&
                   priority_queue[1] == -1) {
            // smaller zone id has higher priority (hospital = 0, industrial = 1)
            if (overloaded_zone < priority_queue[0]) {
                /* New zone has higher priority — bump existing to slot 1 */
                // slide current slot 0 down to slot 1
                priority_queue[1] = priority_queue[0];
                // put the new higher-priority zone in front
                priority_queue[0] = overloaded_zone;
            } else {
                // lower priority zone gets parked in slot 1
                priority_queue[1] = overloaded_zone;
            }
        }
        /* Duplicate entries are silently ignored (queue already has this zone) */

        // unlock so decision engine or others can access state
        pthread_mutex_unlock(&state_mutex);
    }
    return NULL;
}
