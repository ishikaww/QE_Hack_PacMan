#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include "gcp.h"

/* THREAD 6: CLI Thread (prio 20, any cpu) */
void* cli_thread(void* arg) {
    // bump up priority to 20 so cli doesn't lag
    set_thread_priority(20);
    
    // temp buffers to hold input & parsed args
    char  cmd[128];
    char  action[16];
    char  zone[32];
    float load_val;

    // show startup banner
    printf("\n=== Central Controller CLI Active ===\n");

    // main interactive loop
    while (1) {
        printf("GCP-Admin> ");
        fflush(stdout); // force prompt to show up immediately

        // read user input, skip loop if empty/error
        if (fgets(cmd, sizeof(cmd), stdin) == NULL)
            continue;

        // strip off the newline character from the end
        cmd[strcspn(cmd, "\n")] = 0;

        // handle status command
        if (strcmp(cmd, "status") == 0) {
            // lock mut cuz we are reading shared global state
            pthread_mutex_lock(&state_mutex);
            
            printf("\n--- Load Status Table Snapshot ---\n");
            printf("Hospital Load:    %.1fkW\n", global_state.hospital_total);
            printf("Industrial Load:  %.1fkW\n", global_state.industrial_total);
            printf("Residential Load: %.1fkW\n", global_state.residential_total);
            printf("Lighting Load:    %.1fkW\n", global_state.lighting_total);
            printf("----------------------------------\n\n");
            
            // unlock and head back to prompt
            pthread_mutex_unlock(&state_mutex);
            continue;
        }

        /* Width-limited %s prevents buffer overflow into action[16]/zone[32] */
        // try to parse action, zone name, and value. expect exactly 3 items
        if (sscanf(cmd, "%15s %31s %f", action, zone, &load_val) == 3) {
            // check if user wants to 'set' a value
            if (strcmp(action, "set") == 0) {
                // lock mut before touching the command queues
                pthread_mutex_lock(&state_mutex);

                // route load value to the right zone structure
                if (strcmp(zone, "hospital") == 0) {
                    pending_commands[ZONE_HOSPITAL].type         = CMD_UPDATE_LOAD;
                    pending_commands[ZONE_HOSPITAL].target_value = load_val;
                    printf("[CLI] Queued load update of %.1f kW for Hospital\n", load_val);
                } else if (strcmp(zone, "industrial") == 0) {
                    pending_commands[ZONE_INDUSTRIAL].type         = CMD_UPDATE_LOAD;
                    pending_commands[ZONE_INDUSTRIAL].target_value = load_val;
                    printf("[CLI] Queued load update of %.1f kW for Industrial\n", load_val);
                } else if (strcmp(zone, "residential") == 0) {
                    pending_commands[ZONE_RESIDENTIAL].type         = CMD_UPDATE_LOAD;
                    pending_commands[ZONE_RESIDENTIAL].target_value = load_val;
                    printf("[CLI] Queued load update of %.1f kW for Residential\n", load_val);
                } else if (strcmp(zone, "lighting") == 0) {
                    pending_commands[ZONE_LIGHTING].type         = CMD_UPDATE_LOAD;
                    pending_commands[ZONE_LIGHTING].target_value = load_val;
                    printf("[CLI] Queued load update of %.1f kW for Lighting\n", load_val);
                } else {
                    // user typed a typo/invalid zone
                    printf("[CLI] Unknown zone: %s\n", zone);
                }

                // unlock shared state
                pthread_mutex_unlock(&state_mutex);

                // grab de connection id and wake up decision engine if it's active
                int de = de_coid;
                if (de != -1)
                    // send quick qnx pulse to kick the thread awake right now
                    MsgSendPulse(de, SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE_DE_WAKEUP, 0);
            }
        }
    }
    return NULL;
}
