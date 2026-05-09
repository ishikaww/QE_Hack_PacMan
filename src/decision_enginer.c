#include <stdio.h>
#include <sys/neutrino.h>
#include <pthread.h>
#include "gcp.h"

// hard limits for each power zone
#define HOSP_QUOTA    500.0f
#define IND_QUOTA     600.0f
#define LIGHT_QUOTA    80.0f
// reserve limit so we don't pitch black the place
#define LIGHT_RESERVE  14.0f
#define RES_QUOTA     350.0f
// min power res needs to keep basic stuff running
#define RES_RESERVE    50.0f

/* THREAD 2: Decision Engine Thread (prio 62, cpu 1) */
void* decision_engine_thread(void* arg) {
    // lock to core 1 for real-time deterministic behavior
    pin_thread_to_cpu(1);
    // run at high prio so we handle load shedding fast
    set_thread_priority(62);

    // set up qnx channel to receive pulses from other threads
    int chid = ChannelCreate(0);
    // save connection globally so cli/other stuff can kick us
    de_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    struct _pulse pulse;

    while (1) {
        // block until we get a pulse msg, skip on error
        if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) != 0)
            continue;

        // watchdog just pings us to check if we're alive, no logic needed
        if (pulse.code == PULSE_CODE_WD_KICK)
            continue;

        // tell watchdog thread we are still looping fine
        de_alive_flag = 1;

        // lock up state before we read/write shared variables
        pthread_mutex_lock(&state_mutex);

        // check who is next in line to get processed
        int target_zone = priority_queue[0];

        if (target_zone != -1) {
            float current_load, quota;

            // map the current zone to its specific load and threshold
            if (target_zone == ZONE_HOSPITAL) {
                current_load = global_state.hospital_total;
                quota        = HOSP_QUOTA;
            } else if (target_zone == ZONE_INDUSTRIAL) {
                current_load = global_state.industrial_total;
                quota        = IND_QUOTA;
            } else {
                // not hospital/industrial? we don't shed for them, so pop from queue & unlock
                for (int i = 0; i < NUM_ZONES - 1; i++)
                    priority_queue[i] = priority_queue[i + 1];
                priority_queue[NUM_ZONES - 1] = -1;
                pthread_mutex_unlock(&state_mutex);
                continue;
            }

            // calc how much over budget we are
            float deficit = current_load - quota;

            if (deficit > 0) {
                float collected_surplus = 0.0f;

                /* Try lighting first — cap take to what we actually need */
                // check if lights are already degraded/shut down
                if (!zone_is_degraded[ZONE_LIGHTING]) {
                    // see how much we can squeeze out of lights without hitting reserve
                    float available = LIGHT_QUOTA
                                    - global_state.lighting_total
                                    - LIGHT_RESERVE;
                    if (available > 0) {
                        // take only what we need, or whatever is left
                        float take = (available < deficit) ? available : deficit;
                        collected_surplus += take;
                        pending_commands[ZONE_LIGHTING].type = CMD_UPDATE_LOAD;
                        pending_commands[ZONE_LIGHTING].target_value =
                            global_state.lighting_total - take;
                        /* FIX 1: update global_state so next DE wake sees
                           correct value and doesn't re-drain lighting again */
                        // update state immediately so we don't double-dip next loop
                        global_state.lighting_total -= take;
                    }
                }

                /* Try residential if still short — cap take to remaining deficit */
                // if lighting wasn't enough and res isn't degraded yet
                if (collected_surplus < deficit &&
                    !zone_is_degraded[ZONE_RESIDENTIAL]) {
                    // calc what res can give up without dipping below reserve
                    float available = RES_QUOTA
                                    - global_state.residential_total
                                    - RES_RESERVE;
                    if (available > 0) {
                        // find out how much of the deficit is still left
                        float remaining = deficit - collected_surplus;
                        /* FIX 2: only take what's still needed, not all available */
                        // take the smaller of what we can get vs what we actually need
                        float take = (available < remaining) ? available : remaining;
                        collected_surplus += take;
                        pending_commands[ZONE_RESIDENTIAL].type = CMD_UPDATE_LOAD;
                        pending_commands[ZONE_RESIDENTIAL].target_value =
                            global_state.residential_total - take;
                        /* FIX 1 (same): keep global_state coherent */
                        // sync state so we don't over-drain next time
                        global_state.residential_total -= take;
                    }
                }

                // check if we scavenged enough power to save the target zone
                if (collected_surplus >= deficit) {
                    // got enough, just cap the zone to its normal quota limit
                    pending_commands[target_zone].type         = CMD_UPDATE_LOAD;
                    pending_commands[target_zone].target_value = quota;
                } else {
                    // couldn't find enough power, gotta force a full load shed on it
                    pending_commands[target_zone].type = CMD_SHED;
                }
            }

            // pop the processed zone out of the queue and shift others up
            for (int i = 0; i < NUM_ZONES - 1; i++)
                priority_queue[i] = priority_queue[i + 1];
            priority_queue[NUM_ZONES - 1] = -1;
        }

        // unlock state mutex for other threads
        pthread_mutex_unlock(&state_mutex);
    }
    return NULL;
}
