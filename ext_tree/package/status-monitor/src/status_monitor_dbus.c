#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <alsa/asoundlib.h>
#include <dbus/dbus.h>

#define STATUS_FILE "/tmp/system_status.json"
#define MIXER_CACHE_FILE "/tmp/mixer_control_cache"
#define LOCK_FILE "/tmp/status_monitor.lock"
#define DBUS_SERVICE_NAME "org.purefox.statusmonitor"
#define DBUS_OBJECT_PATH "/org/purefox/statusmonitor"
#define DBUS_INTERFACE_NAME "org.purefox.StatusMonitor"

volatile int running = 1;
DBusConnection *dbus_conn = NULL;

void signal_handler(int sig) {
    running = 0;
}

// Structure for storing current state
typedef struct {
    char active_service[32];
    char alsa_state[16];
    int usb_dac;
    char volume[16];
    int muted;
    int volume_control_available;
    int mute_control_available;
    char mixer_control_name[64];  // Found ALSA control name
    int mixer_control_index;      // Control index
    bool mixer_control_valid;     // Is cached control still valid
    time_t last_update;
} system_status_t;

system_status_t current_status = {
    .active_service = "",
    .alsa_state = "unknown",
    .usb_dac = 0,
    .volume = "100%",
    .muted = 0,
    .volume_control_available = 1,
    .mute_control_available = 1,
    .mixer_control_name = "",
    .mixer_control_index = 0,
    .mixer_control_valid = false,
    .last_update = 0
};

// Check active audio process through /proc
void get_active_service(char* service) {
    DIR *proc_dir;
    struct dirent *entry;
    FILE *cmdline_file;
    char path[512];
    char cmdline[1024];
    char *process_name;
    
    const char* services[][2] = {
        {"networkaudiod", "naa"},
        {"raat_app", "raat"},
        {"mpd", "mpd"},
        {"squeeze2upnp", "squeeze2upn"},
        {"ap2renderer", "aprenderer"},
        {"aplayer", "aplayer"},
        {"apscream", "apscream"},
        {"squeezelite", "lms"},
        {"shairport-sync", "shairport"},
        {"librespot", "spotify"},
        {"qobuz-connect", "qobuz"},
        {"tidalconnect", "tidalconnect"},
        {NULL, NULL}
    };
    
    strcpy(service, "");
    
    proc_dir = opendir("/proc");
    if (!proc_dir) return;
    
    while ((entry = readdir(proc_dir)) != NULL) {
        if (!isdigit(entry->d_name[0])) continue;
        if (strlen(entry->d_name) > 100) continue;
        
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        cmdline_file = fopen(path, "r");
        if (!cmdline_file) continue;
        
        if (fgets(cmdline, sizeof(cmdline), cmdline_file)) {
            process_name = strrchr(cmdline, '/');
            if (process_name) {
                process_name++;
            } else {
                process_name = cmdline;
            }
            
            for (int i = 0; services[i][0]; i++) {
                if (strcmp(process_name, services[i][0]) == 0) {
                    strcpy(service, services[i][1]);
                    fclose(cmdline_file);
                    closedir(proc_dir);
                    return;
                }
            }
        }
        fclose(cmdline_file);
    }
    
    closedir(proc_dir);
}

// Check ALSA state
void get_alsa_state(char* state) {
    FILE *fp;
    char buffer[256];
    
    strcpy(state, "unknown");
    
    fp = fopen("/etc/output", "r");
    if (fp) {
        if (fgets(buffer, sizeof(buffer), fp)) {
            buffer[strcspn(buffer, "\n")] = 0;
            if (strcasecmp(buffer, "USB") == 0) {
                strcpy(state, "usb");
            } else if (strcasecmp(buffer, "I2S") == 0) {
                strcpy(state, "i2s");
            }
        }
        fclose(fp);
        return;
    }
    
    fp = fopen("/etc/asound.conf", "r");
    if (fp) {
        while (fgets(buffer, sizeof(buffer), fp)) {
            if (strstr(buffer, "card 1")) {
                strcpy(state, "usb");
                break;
            } else if (strstr(buffer, "card 0")) {
                strcpy(state, "i2s");
                break;
            }
        }
        fclose(fp);
    }
}

// Check USB DAC
int check_usb_dac() {
    struct stat st;
    return (stat("/sys/class/sound/card1", &st) == 0) ? 1 : 0;
}

// Find and cache ALSA mixer control (expensive operation, call only when DAC changes)
bool find_mixer_control() {
    snd_mixer_t *handle;
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid;
    
    strcpy(current_status.mixer_control_name, "");
    current_status.mixer_control_index = 0;
    current_status.mixer_control_valid = false;
    
    if (snd_mixer_open(&handle, 0) < 0) return false;
    if (snd_mixer_attach(handle, "default") < 0) {
        snd_mixer_close(handle);
        return false;
    }
    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) {
        snd_mixer_close(handle);
        return false;
    }
    if (snd_mixer_load(handle) < 0) {
        snd_mixer_close(handle);
        return false;
    }
    
    elem = NULL;
    
    const char* standard_names[] = {
        "PCM", "Speaker", "Master", "Headphone", "Digital", 
        "Playback", "DAC", "Line Out", "Analog", "Output",
        "Front", "Main", "Volume", NULL
    };
    
    for (int i = 0; standard_names[i] != NULL && !elem; i++) {
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_name(sid, standard_names[i]);
        snd_mixer_elem_t* test_elem = snd_mixer_find_selem(handle, sid);
        
        if (test_elem && snd_mixer_selem_has_playback_volume(test_elem)) {
            elem = test_elem;
        }
    }
    
    if (!elem) {
        for (elem = snd_mixer_first_elem(handle); elem; elem = snd_mixer_elem_next(elem)) {
            if (snd_mixer_selem_is_active(elem)) {
                const char *name = snd_mixer_selem_get_name(elem);
                bool has_vol = snd_mixer_selem_has_playback_volume(elem);
                
                if (has_vol && name) {
                    if (strstr(name, "Capture") || strstr(name, "Mic")) {
                        continue;
                    }
                    
                    bool has_valid_channels = snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_FRONT_LEFT) ||
                                            snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_MONO);
                    
                    if (has_valid_channels) {
                        break;
                    }
                }
            }
        }
    }
    
    if (elem) {
        const char *control_name = snd_mixer_selem_get_name(elem);
        if (control_name) {
            strncpy(current_status.mixer_control_name, control_name, sizeof(current_status.mixer_control_name) - 1);
            current_status.mixer_control_name[sizeof(current_status.mixer_control_name) - 1] = '\0';
            current_status.mixer_control_index = snd_mixer_selem_get_index(elem);
            current_status.mixer_control_valid = true;
            
            printf("Found mixer control: '%s',%d\n", 
                   current_status.mixer_control_name, 
                   current_status.mixer_control_index);
        }
    }
    
    snd_mixer_close(handle);
    return current_status.mixer_control_valid;
}

// ALSA API for reading volume (uses cached control)
void get_volume_status_alsa(char* volume, int* muted) {
    snd_mixer_t *handle;
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid;
    long min, max, val;
    int switch_val;
    
    strcpy(volume, "100%");
    *muted = 0;
    
    // If no valid cached control, try to find it
    if (!current_status.mixer_control_valid) {
        if (!find_mixer_control()) {
            return;
        }
    }
    
    if (snd_mixer_open(&handle, 0) < 0) return;
    if (snd_mixer_attach(handle, "default") < 0) {
        snd_mixer_close(handle);
        return;
    }
    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) {
        snd_mixer_close(handle);
        return;
    }
    if (snd_mixer_load(handle) < 0) {
        snd_mixer_close(handle);
        return;
    }
    
    // Use cached control name
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_name(sid, current_status.mixer_control_name);
    snd_mixer_selem_id_set_index(sid, current_status.mixer_control_index);
    elem = snd_mixer_find_selem(handle, sid);
    
    if (!elem) {
        // Cached control not found - invalidate cache and retry
        printf("Cached control '%s',%d not found, invalidating cache\n",
               current_status.mixer_control_name, current_status.mixer_control_index);
        current_status.mixer_control_valid = false;
        snd_mixer_close(handle);
        return;
    }
    
    if (snd_mixer_selem_has_playback_volume(elem)) {
        snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        
        int read_success = 0;
        
        if (snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_MONO)) {
            if (snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO, &val) >= 0) {
                read_success = 1;
            }
        }
        
        if (!read_success && snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_FRONT_LEFT)) {
            if (snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &val) >= 0) {
                read_success = 1;
            }
        }
        
        if (read_success && max > min) {
            int percent = (int)((val - min) * 100 / (max - min));
            snprintf(volume, 16, "%d%%", percent);
        }
    }
    
    if (snd_mixer_selem_has_playback_switch(elem)) {
        if (snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_MONO)) {
            if (snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO, &switch_val) >= 0) {
                *muted = !switch_val;
            }
        } else if (snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_FRONT_LEFT)) {
            if (snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &switch_val) >= 0) {
                *muted = !switch_val;
            }
        }
    }
    
    snd_mixer_close(handle);
}

// Check USB DAC control availability using ALSA API
void check_usb_controls(int* volume_available, int* mute_available) {
    *volume_available = 0;
    *mute_available = 0;
    
    snd_mixer_t *mixer = NULL;
    snd_mixer_elem_t *elem;
    
    if (snd_mixer_open(&mixer, 0) >= 0) {
        if (snd_mixer_attach(mixer, "hw:1") >= 0) {
            if (snd_mixer_selem_register(mixer, NULL, NULL) >= 0) {
                if (snd_mixer_load(mixer) >= 0) {
                    for (elem = snd_mixer_first_elem(mixer); elem; elem = snd_mixer_elem_next(elem)) {
                        if (snd_mixer_selem_is_active(elem)) {
                            const char *name = snd_mixer_selem_get_name(elem);
                            
                            if (name && (strstr(name, "Capture") || strstr(name, "Mic"))) {
                                continue;
                            }
                            
                            if (!*volume_available && snd_mixer_selem_has_playback_volume(elem)) {
                                if (snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_FRONT_LEFT) ||
                                    snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_MONO)) {
                                    *volume_available = 1;
                                }
                            }
                            
                            if (!*mute_available && snd_mixer_selem_has_playback_switch(elem)) {
                                *mute_available = 1;
                            }
                            
                            if (*volume_available && *mute_available) break;
                        }
                    }
                }
            }
        }
        snd_mixer_close(mixer);
    }
}

// Update mixer control cache file
void update_mixer_cache() {
    // Only write cache if we found a valid control
    if (strlen(current_status.mixer_control_name) == 0) {
        printf("No mixer control found, skipping cache update\n");
        return;
    }
    
    FILE *fp = fopen(MIXER_CACHE_FILE, "w");
    if (!fp) {
        printf("WARNING: Cannot open %s for writing: %s\n", MIXER_CACHE_FILE, strerror(errno));
        return;
    }
    
    // Write in format: 'ControlName',index
    fprintf(fp, "'%s',%d\n", current_status.mixer_control_name, current_status.mixer_control_index);
    
    fclose(fp);
    printf("Mixer cache updated: '%s',%d\n", 
           current_status.mixer_control_name, 
           current_status.mixer_control_index);
}

// Update JSON file
void update_status_file() {
    FILE *fp = fopen(STATUS_FILE, "w");
    if (!fp) {
        printf("ERROR: Cannot open %s for writing: %s\n", STATUS_FILE, strerror(errno));
        return;
    }
    
    fprintf(fp, "{\n");
    fprintf(fp, "  \"active_service\": \"%s\",\n", current_status.active_service);
    fprintf(fp, "  \"alsa_state\": \"%s\",\n", current_status.alsa_state);
    fprintf(fp, "  \"usb_dac\": %s,\n", current_status.usb_dac ? "true" : "false");
    fprintf(fp, "  \"volume\": \"%s\",\n", current_status.volume);
    fprintf(fp, "  \"muted\": %s,\n", current_status.muted ? "true" : "false");
    fprintf(fp, "  \"volume_control_available\": %s,\n", current_status.volume_control_available ? "true" : "false");
    fprintf(fp, "  \"mute_control_available\": %s,\n", current_status.mute_control_available ? "true" : "false");
    fprintf(fp, "  \"timestamp\": %ld,\n", current_status.last_update);
    fprintf(fp, "  \"source\": \"dbus_monitor\"\n");
    fprintf(fp, "}\n");
    
    fclose(fp);
    
    // Also update mixer cache
    update_mixer_cache();
}

// Full status update
void refresh_all_status() {
    char old_alsa_state[16];
    int old_usb_dac;
    
    // Save old state to detect DAC changes
    strcpy(old_alsa_state, current_status.alsa_state);
    old_usb_dac = current_status.usb_dac;
    
    get_active_service(current_status.active_service);
    get_alsa_state(current_status.alsa_state);
    current_status.usb_dac = check_usb_dac();
    
    // Detect DAC change (output mode or USB DAC presence changed)
    bool dac_changed = (strcmp(old_alsa_state, current_status.alsa_state) != 0) || 
                       (old_usb_dac != current_status.usb_dac);
    
    if (dac_changed) {
        printf("DAC changed: %s->%s, USB DAC: %d->%d - rescanning controls\n",
               old_alsa_state, current_status.alsa_state, old_usb_dac, current_status.usb_dac);
        
        // Invalidate cached control and force rescan
        current_status.mixer_control_valid = false;
        find_mixer_control();
    }
    
    get_volume_status_alsa(current_status.volume, &current_status.muted);
    
    // Update control availability based on current state
    if (strcmp(current_status.alsa_state, "usb") == 0 && !current_status.usb_dac) {
        current_status.volume_control_available = 0;
        current_status.mute_control_available = 0;
    } else if (strcmp(current_status.alsa_state, "usb") == 0 && current_status.usb_dac) {
        check_usb_controls(&current_status.volume_control_available, &current_status.mute_control_available);
    } else {
        current_status.volume_control_available = 1;
        current_status.mute_control_available = 1;
    }
    
    current_status.last_update = time(NULL);
    
    update_status_file();
    printf("Status updated: service=%s, alsa=%s, volume=%s\n", 
           current_status.active_service, current_status.alsa_state, current_status.volume);
}

// D-Bus signal handler
DBusHandlerResult handle_dbus_message(DBusConnection *connection, DBusMessage *message, void *user_data) {
    const char *interface = dbus_message_get_interface(message);
    const char *member = dbus_message_get_member(message);
    const char *path = dbus_message_get_path(message);
    
    printf("D-Bus signal: %s.%s on %s\n", interface ? interface : "null", 
           member ? member : "null", path ? path : "null");
    
    if (interface && strcmp(interface, DBUS_INTERFACE_NAME) == 0) {
        if (member && strcmp(member, "ServiceChanged") == 0) {
            printf("Service change detected via D-Bus\n");
            char old_alsa_state[16];
            int old_usb_dac;
            
            strcpy(old_alsa_state, current_status.alsa_state);
            old_usb_dac = current_status.usb_dac;
            
            get_active_service(current_status.active_service);
            get_alsa_state(current_status.alsa_state);
            current_status.usb_dac = check_usb_dac();
            
            // Check if DAC changed
            bool dac_changed = (strcmp(old_alsa_state, current_status.alsa_state) != 0) || 
                               (old_usb_dac != current_status.usb_dac);
            
            if (dac_changed) {
                printf("DAC changed from %s (USB:%d) to %s (USB:%d) - rescanning controls\n", 
                       old_alsa_state, old_usb_dac, current_status.alsa_state, current_status.usb_dac);
                
                // Invalidate cache and rescan
                current_status.mixer_control_valid = false;
                find_mixer_control();
                
                // Recalculate control availability
                if (strcmp(current_status.alsa_state, "usb") == 0 && !current_status.usb_dac) {
                    current_status.volume_control_available = 0;
                    current_status.mute_control_available = 0;
                } else if (strcmp(current_status.alsa_state, "usb") == 0 && current_status.usb_dac) {
                    check_usb_controls(&current_status.volume_control_available, 
                                     &current_status.mute_control_available);
                } else {
                    current_status.volume_control_available = 1;
                    current_status.mute_control_available = 1;
                }
            }
            
            current_status.last_update = time(NULL);
            update_status_file();
        } else if (member && strcmp(member, "VolumeChanged") == 0) {
            printf("Volume change detected via D-Bus\n");
            get_volume_status_alsa(current_status.volume, &current_status.muted);
            current_status.last_update = time(NULL);
            update_status_file();
        }
    }
    
    if (interface && strstr(interface, "systemd")) {
        printf("SystemD signal detected, refreshing services\n");
        get_active_service(current_status.active_service);
        get_alsa_state(current_status.alsa_state);
        current_status.last_update = time(NULL);
        update_status_file();
    }
    
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// D-Bus initialization
int init_dbus() {
    DBusError error;
    dbus_error_init(&error);
    
    dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error)) {
        printf("D-Bus connection error: %s\n", error.message);
        dbus_error_free(&error);
        return -1;
    }
    
    dbus_bus_request_name(dbus_conn, DBUS_SERVICE_NAME, 
                         DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error)) {
        printf("D-Bus name request error: %s\n", error.message);
        dbus_error_free(&error);
        return -1;
    }
    
    char match_rule[512];
    snprintf(match_rule, sizeof(match_rule), 
             "type='signal',interface='%s'", DBUS_INTERFACE_NAME);
    dbus_bus_add_match(dbus_conn, match_rule, &error);
    
    dbus_bus_add_match(dbus_conn, 
                      "type='signal',interface='org.freedesktop.systemd1.Manager'", 
                      &error);
    
    dbus_connection_add_filter(dbus_conn, handle_dbus_message, NULL, NULL);
    
    printf("D-Bus initialized successfully\n");
    return 0;
}

int main() {
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    FILE *fp = fopen(LOCK_FILE, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }
    
    printf("D-Bus Status Monitor started (PID: %d)\n", getpid());
    
    if (init_dbus() < 0) {
        printf("Failed to initialize D-Bus, falling back to polling mode\n");
        while (running) {
            refresh_all_status();
            sleep(2);
        }
    } else {
        refresh_all_status();
        
        printf("Entering D-Bus event loop\n");
        while (running) {
            dbus_connection_read_write_dispatch(dbus_conn, 1000);
            
            static time_t last_alsa_check = 0;
            static time_t last_full_refresh = 0;
            time_t now = time(NULL);
            
            if (now - last_alsa_check > 1) {
                char old_volume[16];
                int old_muted = current_status.muted;
                strcpy(old_volume, current_status.volume);
                
                get_volume_status_alsa(current_status.volume, &current_status.muted);
                
                if (strcmp(old_volume, current_status.volume) != 0 || old_muted != current_status.muted) {
                    printf("Volume changed: %s (muted: %s)\n", current_status.volume, current_status.muted ? "yes" : "no");
                    current_status.last_update = now;
                    update_status_file();
                }
                last_alsa_check = now;
            }
            
            if (now - last_full_refresh > 30) {
                refresh_all_status();
                last_full_refresh = now;
            }
        }
        
        if (dbus_conn) {
            dbus_connection_close(dbus_conn);
        }
    }
    
    unlink(LOCK_FILE);
    printf("D-Bus Status Monitor stopped\n");
    
    return 0;
}
