#ifndef GCP_H
#define GCP_H

// header guards so compiler doesnt freak out if imported twice
#include <stdint.h>
#include <sys/neutrino.h>
#include <pthread.h>

// pulse IDs for lightweight qnx signalling between threads
#define PULSE_CODE_DE_WAKEUP  10 // tell decision engine to run
#define PULSE_CODE_ZONE_PING  11 // keepalive heartbeat from zone
#define PULSE_CODE_WD_KICK    12 // watchdog pat
#define PULSE_CODE_RECOVER    13 // power recovery pulse
#define PULSE_CODE_LOG_EVENT  14 // ping the logger thread
#define PULSE_CODE_QM_WAKEUP  15 // queue manager wakeup

// simple enum to map zones to array indexes
typedef enum {
    ZONE_HOSPITAL = 0,
    ZONE_INDUSTRIAL,
    ZONE_RESIDENTIAL,
    ZONE_LIGHTING,
    NUM_ZONES
} ZoneID;

// action codes we send back to the zone relays
typedef enum {
    CMD_NONE = 0,     // do nothing
    CMD_SHED,         // kill power to this zone
    CMD_RESTORE,      // bring zone back online
    CMD_UPDATE_LOAD   // cap load at new limit
} CmdType;

// telemetry payload sent from zones to main controller
typedef struct {
    ZoneID   zone_id;
    float    current_load; // in kW
    uint8_t  relay_status; // 1 = on, 0 = tripped
    uint64_t timestamp;    // cycle count
} TelemetryMsg;

// payload we send back to zones in the reply message
typedef struct {
    CmdType type;
    float   target_value; // target kW limit if updating load
} CommandMsg;

// snapshot of current power usage across all zones
typedef struct {
    float hospital_total;
    float industrial_total;
    float residential_total;
    float lighting_total;
} SystemState;

/* GCP global state — defined in gcp.c, visible to all translation units */
// shared global state accessed by different threads (needs state_mutex lock!)
extern SystemState       global_state;
extern pthread_mutex_t   state_mutex;
extern int               gcp_chid;       // main channel id
extern volatile int      de_coid;        // decision engine conn id
extern volatile int      logger_coid;    // logger conn id
extern volatile int      q_manager_coid; // queue manager conn id
extern CommandMsg        pending_commands[NUM_ZONES]; // outbound command buffer
extern volatile uint8_t  de_alive_flag;  // pet flag for watchdog
extern volatile uint64_t zone_last_seen[NUM_ZONES]; // heartbeat timestamps
extern uint8_t           zone_is_degraded[NUM_ZONES]; // track who is dimmed/shedded
extern int               priority_queue[NUM_ZONES];   // zones waiting for shed logic

// helper prototypes
void pin_thread_to_cpu(int cpu);
void set_thread_priority(int priority);

#endif
