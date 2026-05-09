#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <unistd.h>
#include "gcp.h"

// global system state holding current load numbers
SystemState             global_state;
// lock for protecting global state and shared arrays
pthread_mutex_t         state_mutex    = PTHREAD_MUTEX_INITIALIZER;
// channel id for the main message receiver
int                     gcp_chid;
// coids for sending messages/pulses to other threads
volatile int            de_coid        = -1;
volatile int            logger_coid    = -1;
volatile int            q_manager_coid = -1;
// queue of pending commands to reply to zones with
CommandMsg              pending_commands[NUM_ZONES];
// flag for watchdog to see if decision engine is still kicking
volatile uint8_t        de_alive_flag  = 1;
// timestamp of last heartbeat/telemetry from each zone
volatile uint64_t       zone_last_seen[NUM_ZONES];
// tracks if a zone is in degraded mode or not
uint8_t                 zone_is_degraded[NUM_ZONES] = {0};
// simple priority queue for load shedding
int                     priority_queue[NUM_ZONES]   = {-1, -1, -1, -1};

// force this thread to run on a specific core
void pin_thread_to_cpu(int cpu) {
    unsigned mask = 1 << cpu;
    ThreadCtl(_NTO_TCTL_RUNMASK, (void*)(uintptr_t)mask);
}

// change thread scheduling to fifo and set priority
void set_thread_priority(int priority) {
    struct sched_param param;
    param.sched_priority = priority;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
}

// external thread handlers defined in other files
extern void* decision_engine_thread(void* arg);
extern void* priority_queue_thread(void* arg);
extern void* zone_health_monitor_thread(void* arg);
extern void* internal_watchdog_thread(void* arg);
extern void* cli_thread(void* arg);
extern void* logger_thread(void* arg);

/* THREAD 1: Message Dispatch Thread (prio 63, cpu 1) */
void* msg_dispatch_thread(void* arg) {
    // lock this thread to cpu 1 for real-time latency control
    pin_thread_to_cpu(1);
    // run at max priority so we never drop telemetry msgs
    set_thread_priority(63);

    // buffer for incoming zone telemetry
    TelemetryMsg msg;
    // receive id for qnx replies
    int rcvid;

    // open up the main ipc channel
    gcp_chid = ChannelCreate(0);
    printf("[gcp dispatch] channel %d active\n", gcp_chid);

    // main dispatch loop
    while (1) {
        // block waiting for telemetry messages from the zones
        rcvid = MsgReceive(gcp_chid, &msg, sizeof(msg), NULL);
        // if rcvid is positive, it's a valid message we need to handle
        if (rcvid > 0) {
            // lock state so we can update telemetry safely
            pthread_mutex_lock(&state_mutex);

            // save arrival timestamp in cpu cycles
            zone_last_seen[msg.zone_id] = ClockCycles();

            // process load based on which zone sent the telemetry
            if (msg.zone_id == ZONE_HOSPITAL) {
                global_state.hospital_total = msg.current_load;
                int qm = q_manager_coid;
                // if hospital is over limit, ping queue manager to sort it out
                if (msg.current_load > 500.0f && qm != -1) {
                    MsgSendPulse(qm, SIGEV_PULSE_PRIO_INHERIT,
                                 PULSE_CODE_QM_WAKEUP, ZONE_HOSPITAL);
                }
            } else if (msg.zone_id == ZONE_INDUSTRIAL) {
                global_state.industrial_total = msg.current_load;
                int qm = q_manager_coid;
                // industrial over limit, trigger queue manager
                if (msg.current_load > 600.0f && qm != -1) {
                    MsgSendPulse(qm, SIGEV_PULSE_PRIO_INHERIT,
                                 PULSE_CODE_QM_WAKEUP, ZONE_INDUSTRIAL);
                }
            } else if (msg.zone_id == ZONE_RESIDENTIAL) {
                global_state.residential_total = msg.current_load;
            } else if (msg.zone_id == ZONE_LIGHTING) {
                global_state.lighting_total = msg.current_load;
            }

            // grab any pending command we have waiting for this specific zone
            CommandMsg cmd = pending_commands[msg.zone_id];
            // clear it out so we don't send duplicate commands next time
            pending_commands[msg.zone_id].type = CMD_NONE;

            // release lock before doing the blocking reply
            pthread_mutex_unlock(&state_mutex);

            // send command back to the zone in the reply payload
            MsgReply(rcvid, 0, &cmd, sizeof(cmd));

            // tell decision engine to wake up and check if things need adjustment
            int de = de_coid;
            if (de != -1) {
                MsgSendPulse(de, SIGEV_PULSE_PRIO_INHERIT,
                             PULSE_CODE_DE_WAKEUP, 0);
            }
        }
    }
    return NULL;
}

int main(void) {
    // print startup banner with pid
    printf("[gcp] starting controller process (PID: %d)...\n", getpid());

    // set up default/starting load levels for each zone
    global_state.hospital_total    = 400.0f;
    global_state.industrial_total  = 480.0f;
    global_state.residential_total = 180.0f;
    global_state.lighting_total    =  45.0f;

    // init last seen tracker to current time so monitor doesn't instantly trip
    uint64_t now = ClockCycles();
    for (int i = 0; i < NUM_ZONES; i++) {
        zone_last_seen[i] = now;
    }

    // thread handles for all the workers
    pthread_t t1, t2, t3, t4, t5, t6, t7;

    // spin up all helper and worker threads
    pthread_create(&t1, NULL, msg_dispatch_thread,        NULL);
    pthread_create(&t2, NULL, decision_engine_thread,     NULL);
    pthread_create(&t3, NULL, priority_queue_thread,      NULL);
    pthread_create(&t4, NULL, zone_health_monitor_thread, NULL);
    pthread_create(&t5, NULL, internal_watchdog_thread,   NULL);
    pthread_create(&t6, NULL, cli_thread,                 NULL);
    pthread_create(&t7, NULL, logger_thread,              NULL);

    // block here and wait for threads (basically run forever)
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);
    pthread_join(t5, NULL);
    pthread_join(t6, NULL);
    pthread_join(t7, NULL);

    return 0;
}
