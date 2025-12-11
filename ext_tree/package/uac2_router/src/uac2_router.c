#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#define UAC2_CARD "hw:1,0"
#define I2S_CARD "hw:0,0"
#define PERIOD_FRAMES 1024  /* Optimal period size that both devices accept */

/* New sysfs interface from the modified u_audio.c driver */
#define SYSFS_UAC2_PATH "/sys/class/u_audio"
#define SYSFS_RATE_FILE "rate"
#define SYSFS_FORMAT_FILE "format"
#define SYSFS_CHANNELS_FILE "channels"

/* Fixed format for I2S (as in the original PureCore) */
#define I2S_FORMAT_PCM SND_PCM_FORMAT_S32_LE  /* PCM: 32-bit */
#define I2S_FORMAT_DSD SND_PCM_FORMAT_DSD_U32_LE  /* DSD: 32-bit DSD */
#define I2S_CHANNELS 2                     /* Always stereo */

/* DSD sample rates (native DSD64/128/256/512) */
#define DSD64_RATE   2822400
#define DSD128_RATE  5644800
#define DSD256_RATE  11289600
#define DSD512_RATE  22579200

/* Buffer size for netlink uevent */
#define UEVENT_BUFFER_SIZE 4096

static volatile int running = 1;

/* Global variables for signal handler */
static int uevent_sock = -1;

/* Global PCM handles for signal handler */
static snd_pcm_t *pcm_capture = NULL;
static snd_pcm_t *pcm_playback = NULL;

/* Global UAC card info for signal handler */
static char uac_card_path[256] = {0};
static char uac_card_name[32] = {0};

/* Simple recovery counter from v4.0 */
static int recovery_counter = 0;
static int consecutive_errors = 0;

static void sighandler(int sig) {
    printf("\n[STOP] Received signal %d, shutting down gracefully...\n", sig);
    running = 0;
    
    /* Close PCM devices gracefully */
    if (pcm_capture) {
        snd_pcm_drain(pcm_capture);
        snd_pcm_close(pcm_capture);
        pcm_capture = NULL;
    }
    
    if (pcm_playback) {
        snd_pcm_drain(pcm_playback);
        snd_pcm_close(pcm_playback);
        pcm_playback = NULL;
    }
    
    /* Close netlink socket */
    if (uevent_sock >= 0) {
        close(uevent_sock);
        uevent_sock = -1;
    }
    
    /* Clear UAC card info */
    memset(uac_card_path, 0, sizeof(uac_card_path));
    memset(uac_card_name, 0, sizeof(uac_card_name));
    
    printf("[STOP] Cleanup completed\n");
}

/* Determine if frequency is DSD */
static int is_dsd_rate(unsigned int rate) {
    return (rate == DSD64_RATE || rate == DSD128_RATE ||
            rate == DSD256_RATE || rate == DSD512_RATE);
}

/* Get DSD format name by frequency */
static const char* get_dsd_name(unsigned int rate) {
    switch (rate) {
        case DSD64_RATE:  return "DSD64";
        case DSD128_RATE: return "DSD128";
        case DSD256_RATE: return "DSD256";
        case DSD512_RATE: return "DSD512";
        default: return "Unknown";
    }
}

/* Find UAC card in /sys/class/u_audio/ */
static int find_uac_card(void) {
    char path[256];
    struct stat st;

    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "%s/uac_card%d", SYSFS_UAC2_PATH, i);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(uac_card_path, sizeof(uac_card_path), "%s", path);
            snprintf(uac_card_name, sizeof(uac_card_name), "uac_card%d", i);
            printf("Found UAC card: %s (card %d)\n", uac_card_path, i);
            return i;
        }
    }

    fprintf(stderr, "ERROR: No UAC card found in %s\n", SYSFS_UAC2_PATH);
    return -1;
}

/* Read value from sysfs file */
static int read_sysfs_int(const char *filename) {
    char path[512];
    FILE *fp;
    int value = 0;

    snprintf(path, sizeof(path), "%s/%s", uac_card_path, filename);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    if (fscanf(fp, "%d", &value) != 1) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return value;
}

/* Create netlink socket for uevent */
static int create_uevent_socket(void) {
    struct sockaddr_nl addr;
    int sock;

    sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sock < 0) {
        fprintf(stderr, "Cannot create netlink socket: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1;  /* Kernel events */
    addr.nl_pid = getpid();

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot bind netlink socket: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    return sock;
}

/* Close PCM devices */
static void close_pcms(void) {
    if (pcm_capture) {
        snd_pcm_drop(pcm_capture);
        snd_pcm_close(pcm_capture);
        pcm_capture = NULL;
    }
    if (pcm_playback) {
        snd_pcm_drop(pcm_playback);
        snd_pcm_close(pcm_playback);
        pcm_playback = NULL;
    }
}

/* Configure PCM device - ORIGINAL WORKING SETUP with stable recovery */
static int setup_pcm(snd_pcm_t **pcm, const char *device, snd_pcm_stream_t stream,
                     unsigned int rate, snd_pcm_format_t format, unsigned int channels)
{
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    int err;
    /* Fixed period size for all rates - both devices must match */
    snd_pcm_uframes_t period_size = 512;  /* Fixed for capture/playback sync */
    snd_pcm_uframes_t buffer_size = period_size * 8;  /* Larger buffer for stability */

    if ((err = snd_pcm_open(pcm, device, stream, 0)) < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, snd_strerror(err));
        return err;
    }

    /* Hardware parameters */
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(*pcm, hw_params);
    snd_pcm_hw_params_set_access(*pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(*pcm, hw_params, format);
    snd_pcm_hw_params_set_channels(*pcm, hw_params, channels);
    snd_pcm_hw_params_set_rate_near(*pcm, hw_params, &rate, 0);

    /* Set explicit period and buffer sizes */
    snd_pcm_hw_params_set_period_size_near(*pcm, hw_params, &period_size, 0);
    snd_pcm_hw_params_set_buffer_size_near(*pcm, hw_params, &buffer_size);

    if ((err = snd_pcm_hw_params(*pcm, hw_params)) < 0) {
        fprintf(stderr, "Cannot set hw params for %s: %s\n", device, snd_strerror(err));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return err;
    }

    snd_pcm_hw_params_get_period_size(hw_params, &period_size, 0);
    snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_size);
    snd_pcm_hw_params_get_rate(hw_params, &rate, 0);

    /* Software parameters */
    snd_pcm_sw_params_alloca(&sw_params);
    snd_pcm_sw_params_current(*pcm, sw_params);
    snd_pcm_sw_params_set_start_threshold(*pcm, sw_params, buffer_size / 2);
    snd_pcm_sw_params_set_avail_min(*pcm, sw_params, period_size);
    snd_pcm_sw_params(*pcm, sw_params);

    printf("  %s: %u Hz, %s, %u ch, period %lu, buffer %lu frames\n",
           device, rate, snd_pcm_format_name(format), channels, period_size, buffer_size);

    return 0;
}

/* Configure audio with given frequency - ORIGINAL WORKING SETUP */
static int configure_audio(unsigned int rate, int card, char **buffer, size_t *buffer_size,
                          snd_pcm_uframes_t *period_size_out) {
    snd_pcm_uframes_t capture_period_size, playback_period_size;
    int is_dsd = is_dsd_rate(rate);
    snd_pcm_format_t i2s_format = is_dsd ? I2S_FORMAT_DSD : I2S_FORMAT_PCM;

    if (is_dsd) {
        printf("\n[CONFIG] ═══ DSD MODE: %s (%u Hz) ═══\n", get_dsd_name(rate), rate);
    } else {
        printf("\n[CONFIG] Setting up PCM audio: %u Hz, 32-bit, Stereo\n", rate);
    }

    close_pcms();

    /* UAC2 capture - ALWAYS use PCM S32_LE format
     * UAC2 gadget RAW_DATA (Alt Setting 2) transmits DSD as raw 32-bit data,
     * which ALSA sees as PCM format. We convert to DSD for I2S if needed. */
    char uac_device[32];
    sprintf(uac_device, "hw:%d,0", card);
    if (setup_pcm(&pcm_capture, uac_device, SND_PCM_STREAM_CAPTURE,
                  rate, I2S_FORMAT_PCM, I2S_CHANNELS) < 0) {
        return -1;
    }

    /* I2S playback - PCM or DSD format depending on frequency */
    if (setup_pcm(&pcm_playback, I2S_CARD, SND_PCM_STREAM_PLAYBACK,
                  rate, i2s_format, I2S_CHANNELS) < 0) {
        close_pcms();
        return -1;
    }

    if (is_dsd) {
        printf("[CONFIG] DSD stream configured: USB→I2S routing active\n");
    }

    /* Get period sizes for both devices */
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);

    snd_pcm_hw_params_current(pcm_capture, hw);
    snd_pcm_hw_params_get_period_size(hw, &capture_period_size, 0);

    snd_pcm_hw_params_current(pcm_playback, hw);
    snd_pcm_hw_params_get_period_size(hw, &playback_period_size, 0);

    /* Use smaller period for reading */
    *period_size_out = capture_period_size;

    /* Allocate buffer with size of larger period (for data accumulation) */
    snd_pcm_uframes_t max_period = (capture_period_size > playback_period_size) ?
                                    capture_period_size : playback_period_size;
    /* Use PCM format for buffer calculation (DSD and PCM both 32-bit) */
    size_t frame_bytes = snd_pcm_format_physical_width(I2S_FORMAT_PCM) / 8 * I2S_CHANNELS;
    *buffer_size = max_period * frame_bytes;

    /* Safe realloc - avoid memory leak */
    char *new_buffer = realloc(*buffer, *buffer_size);
    if (!new_buffer) {
        fprintf(stderr, "Cannot allocate buffer\n");
        free(*buffer);  /* Free old buffer */
        *buffer = NULL;
        close_pcms();
        return -1;
    }
    *buffer = new_buffer;

    /* Prepare both devices */
    if (snd_pcm_prepare(pcm_capture) < 0) {
        fprintf(stderr, "Cannot prepare capture\n");
        close_pcms();
        return -1;
    }

    if (snd_pcm_prepare(pcm_playback) < 0) {
        fprintf(stderr, "Cannot prepare playback\n");
        close_pcms();
        return -1;
    }

    /* Start capture (playback will start automatically on first write) */
    if (snd_pcm_start(pcm_capture) < 0) {
        fprintf(stderr, "Cannot start capture\n");
        close_pcms();
        return -1;
    }

    printf("[CONFIG] Audio configured successfully\n\n");

    return 0;
}

int main(void) {
    char *buffer = NULL;
    size_t buffer_size = 0;
    unsigned int current_rate = 0;
    snd_pcm_uframes_t period_size = PERIOD_FRAMES;
    char uevent_buf[UEVENT_BUFFER_SIZE];
    int uac_card = -1;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  UAC2 -> I2S Router v2.3\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Find UAC card */
    uac_card = find_uac_card();
    if (uac_card < 0) {
        fprintf(stderr, "ERROR: UAC2 device not found. Is gadget loaded?\n");
        return 1;
    }

    /* Create netlink socket for uevent */
    uevent_sock = create_uevent_socket();
    if (uevent_sock < 0) {
        fprintf(stderr, "ERROR: Cannot create uevent socket\n");
        return 1;
    }

    printf("Listening for kobject uevent from u_audio driver...\n");
    printf("Waiting for rate changes...\n\n");

    /* Read initial frequency and configure audio */
    int rate = read_sysfs_int(SYSFS_RATE_FILE);
    if (rate > 0) {
        printf("Initial rate: %d Hz\n", rate);
        if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
            current_rate = rate;
        }
    }

    /* Main loop - ORIGINAL WORKING LOOP with v4.0 SIMPLE recovery */
    while (running) {
        /* Non-blocking uevent check (MSG_DONTWAIT is very fast if no event) */
        ssize_t len = recv(uevent_sock, uevent_buf, sizeof(uevent_buf) - 1, MSG_DONTWAIT);
        if (len > 0) {
            uevent_buf[len] = '\0';

            /* Check that this is an event from our UAC card */
            if (strstr(uevent_buf, "u_audio") && strstr(uevent_buf, uac_card_name)) {
                /* Read new frequency */
                rate = read_sysfs_int(SYSFS_RATE_FILE);

                if (rate > 0 && rate != current_rate) {
                    printf("\n[CHANGE] Rate changed: %u Hz -> %d Hz\n", current_rate, rate);

                    if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
                        current_rate = rate;
                    }
                }
            }
        }

        /* Audio routing - SIMPLE v4.0 approach */
        if (!pcm_capture || !pcm_playback) {
            usleep(100000);  /* 100ms */
            continue;
        }

        /* ENHANCED: Non-blocking read with proper error handling */
        snd_pcm_sframes_t frames;

        /* First try non-blocking read */
        frames = snd_pcm_readi(pcm_capture, buffer, period_size);

        if (frames == -EAGAIN) {
            /* No data available - brief sleep and continue */
            usleep(1000);  /* 1ms minimal delay */
            recovery_counter = 0;
            continue;
        }

        if (frames < 0) {
            consecutive_errors++;

            /* SIMPLE v4.0 recovery */
            if (frames == -EPIPE) {
                fprintf(stderr, "[RECOVER] Capture XRUN at %u Hz\n", current_rate);
                snd_pcm_prepare(pcm_capture);
                if (snd_pcm_start(pcm_capture) == 0) {
                    consecutive_errors = 0;
                    usleep(5000);  /* 5ms recovery */
                    continue;
                }
            }

            if (consecutive_errors >= 5) {  /* Fast recovery */
                fprintf(stderr, "[RECOVERY] Quick recovery after %d errors\n", consecutive_errors);

                /* Fast recovery sequence */
                if (pcm_capture) {
                    snd_pcm_drop(pcm_capture);
                    if (snd_pcm_prepare(pcm_capture) == 0) {
                        snd_pcm_start(pcm_capture);
                    }
                }

                if (pcm_playback) {
                    snd_pcm_state_t state = snd_pcm_state(pcm_playback);
                    if (state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_PREPARED) {
                        snd_pcm_prepare(pcm_playback);
                    }
                }

                consecutive_errors = 0;
                usleep(10000);  /* 10ms brief pause */
                continue;
            }

            fprintf(stderr, "[ERROR] Read failed: %s (error #%d)\n", snd_strerror(frames), consecutive_errors);
            usleep(10000);
            continue;
        }

        if (frames > 0) {
            /* Reset error counter on successful read */
            consecutive_errors = 0;

            /* ENHANCED: Non-blocking playback with immediate recovery */
            snd_pcm_sframes_t written = snd_pcm_writei(pcm_playback, buffer, frames);
            if (written < 0) {
                /* SIMPLE v4.0 recovery */
                if (written == -EPIPE) {
                    fprintf(stderr, "[RECOVER] Playback XRUN at %u Hz\n", current_rate);
                    snd_pcm_prepare(pcm_playback);
                    written = snd_pcm_writei(pcm_playback, buffer, frames);
                    if (written >= 0) {
                        continue;
                    }
                }

                fprintf(stderr, "[ERROR] Write failed: %s\n", snd_strerror(written));
                continue;
            }
        }
    }

    /* Cleanup */
    if (uevent_sock >= 0)
        close(uevent_sock);

    if (buffer)
        free(buffer);

    close_pcms();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  UAC2 -> I2S Router v2.3 stopped\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}