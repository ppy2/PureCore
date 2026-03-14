#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>

#define UAC2_CARD "hw:1,0"
#define I2S_CARD "hw:0,0"
#define PERIOD_FRAMES 2048  /* Large period for XRUN protection (music listening) */

/* New sysfs interface from the modified u_audio.c driver */
#define SYSFS_UAC2_PATH "/sys/class/u_audio"
#define SYSFS_RATE_FILE "rate"
#define SYSFS_FORMAT_FILE "format"
#define SYSFS_CHANNELS_FILE "channels"
#define SYSFS_FEEDBACK_FILE "feedback"

/* Fixed format for I2S (upgraded to 8-channel 7.1 surround) */
#define I2S_FORMAT_PCM SND_PCM_FORMAT_S32_LE  /* PCM: 32-bit */
#define I2S_FORMAT_DSD SND_PCM_FORMAT_DSD_U32_LE  /* DSD: 32-bit DSD */
#define I2S_CHANNELS 2                     /* Back to stereo until multi-channel I2S is fully implemented */

/* DSD sample rates — native DSD bit rates, 44.1kHz family */
#define DSD64_RATE    2822400
#define DSD128_RATE   5644800
#define DSD256_RATE  11289600
#define DSD512_RATE  22579200

/* DSD sample rates — native DSD bit rates, 48kHz family */
#define DSD64_RATE_48    3072000
#define DSD128_RATE_48   6144000
#define DSD256_RATE_48  12288000
#define DSD512_RATE_48  24576000

/* Buffer size for netlink uevent */
#define UEVENT_BUFFER_SIZE 4096

static volatile int running = 1;
static snd_pcm_t *pcm_capture = NULL;
static snd_pcm_t *pcm_playback = NULL;
static char uac_card_path[256] = "";
static char uac_card_name[64] = "";
static int consecutive_errors = 0;
static int i2s_started = 0;  /* Track if I2S playback has been started */
static int dsd_dump_done = 0; /* DSD data dump flag */
static int is_current_dsd = 0; /* Current mode is DSD */
static int pitch_load_pending = 0; /* Deferred pitch load after stream starts */
static unsigned int pitch_load_value = 0; /* Value to load */

/* Byte-swap macro for DSD 32-bit words.
 * USB RAW_DATA sends DSD bytes oldest-first: [B0(oldest) B1 B2 B3(newest)]
 * ALSA DSD_U32_LE stores oldest at MSB (byte 3): [B0(newest) B1 B2 B3(oldest)]
 * Without bswap, I2S VDW=16 + dsd_sample_swap sends bytes reversed → violet noise. */
#define BSWAP32(x) (((x) >> 24) | (((x) >> 8) & 0xFF00) | \
                    (((x) << 8) & 0xFF0000) | ((x) << 24))
#define MAX_CONSECUTIVE_ERRORS 50  /* After 50 consecutive errors (~0.5 sec) - reopen PCM */

/* Volume synchronization */
static snd_mixer_t *uac2_mixer = NULL;
static snd_mixer_t *i2s_mixer = NULL;
static snd_mixer_elem_t *uac2_volume_elem = NULL;
static snd_mixer_elem_t *i2s_volume_elem = NULL;
static long last_uac2_volume = -1;
static int last_uac2_mute = -1;

/* Elastic buffer management for drift compensation */
typedef struct {
    snd_pcm_uframes_t target_size;      /* Target buffer size */
    snd_pcm_uframes_t min_size;         /* Minimum buffer size */
    snd_pcm_uframes_t max_size;         /* Maximum buffer size */
    long drift_trend;                   /* Trend prediction accumulator */
    long drift_history[5];              /* Last 5 drift measurements */
    int history_index;                  /* Circular buffer index */
    unsigned long last_micro_check;     /* Last micro-adjustment time */
    unsigned long last_macro_check;     /* Last macro-adjustment time */
    int stability_counter;              /* Stability tracking */
} elastic_buffer_t;

static elastic_buffer_t ebuf = {0};
static unsigned long last_drift_check_time = 0;
static long cumulative_drift = 0;  /* accumulated drift in samples */
static unsigned int current_period_size = PERIOD_FRAMES;

#define MICRO_CHECK_INTERVAL 5      /* Micro-adjustment every 5 seconds */
#define MACRO_CHECK_INTERVAL 30     /* Macro-adjustment every 30 seconds */
#define DRIFT_THRESHOLD 5           /* 5 samples threshold */
#define BUFFER_MIN_SIZE (PERIOD_FRAMES * 2)    /* Minimum buffer */
#define BUFFER_MAX_SIZE (PERIOD_FRAMES * 32)   /* Maximum buffer */
#define BUFFER_TARGET_SIZE (PERIOD_FRAMES * 4) /* Target buffer */
#define STABILITY_THRESHOLD 10      /* Stable after 10 checks */

/* Forward declaration — defined later in file */
static int read_sysfs_int(const char *filename);

/* Pitch learning: track running average of feedback for save/restore.
 * Uses U-Boot env variable "feedback_pitch" (works on squashfs too).
 * Saves only when oscillation has settled (min/max range < threshold). */
static long pitch_ema = 0;          /* EMA of pitch × 256 (fixed-point) */
static int pitch_samples = 0;       /* Number of samples collected */
static unsigned int pitch_last_saved = 0; /* Last value written to NAND */
static unsigned long pitch_loop_counter = 0;  /* Main loop iteration counter */
static int pitch_min = 1001000;     /* Min pitch in current window */
static int pitch_max = 999000;      /* Max pitch in current window */
static int pitch_window_count = 0;  /* Samples in current window */
#define PITCH_READ_INTERVAL 500     /* Read sysfs every N iterations (~1 sec) */
#define PITCH_SETTLE_SAMPLES 60     /* Need ~60 samples (~1 min) in window */
#define PITCH_EMA_ALPHA 8           /* EMA alpha = 1/8 (slow tracking) */
#define PITCH_STABLE_RANGE 150      /* Max min-max range to consider stable (PPM) */
#define PITCH_SAVE_THRESHOLD 50     /* Only write NAND if delta > 50 PPM */

/* Load saved pitch from U-Boot env (deferred — written after stream starts) */
static void load_saved_pitch(void) {
    FILE *pp = popen("fw_printenv -n feedback_pitch 2>/dev/null", "r");
    if (!pp) return;

    unsigned int saved_pitch = 0;
    if (fscanf(pp, "%u", &saved_pitch) == 1 &&
        saved_pitch >= 999000 && saved_pitch <= 1001000) {
        pitch_load_pending = 1;
        pitch_load_value = saved_pitch;
        pitch_ema = (long)saved_pitch * 256;
        pitch_samples = PITCH_SETTLE_SAMPLES;  /* Already settled */
        pitch_last_saved = saved_pitch;
        printf("[PITCH] Loaded pitch %u from U-Boot env (will apply after stream starts)\n",
               saved_pitch);
    }
    pclose(pp);
}

/* Apply saved pitch to sysfs (call after first successful read) */
static void apply_saved_pitch(void) {
    if (!pitch_load_pending) return;
    pitch_load_pending = 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", uac_card_path, SYSFS_FEEDBACK_FILE);
    FILE *fw = fopen(path, "w");
    if (fw) {
        fprintf(fw, "%u", pitch_load_value);
        fclose(fw);
        printf("[PITCH] Applied saved pitch %u to sysfs (PI frozen for 5 min)\n",
               pitch_load_value);
    }
}

/* Save current pitch average to U-Boot env (only if significantly changed) */
static void save_pitch(void) {
    if (pitch_samples < PITCH_SETTLE_SAMPLES) return;

    unsigned int avg_pitch = (unsigned int)(pitch_ema / 256);
    if (avg_pitch < 999000 || avg_pitch > 1001000) return;

    /* Only write NAND if value changed significantly */
    int delta = (int)avg_pitch - (int)pitch_last_saved;
    if (delta < 0) delta = -delta;
    if (pitch_last_saved != 0 && delta <= PITCH_SAVE_THRESHOLD) return;

    /* Run fw_setenv in background — NAND write blocks for tens of ms,
     * which causes XRUN at high sample rates (384kHz period=1.3ms). */
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%u", avg_pitch);
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: write to NAND and exit */
        execlp("fw_setenv", "fw_setenv", "feedback_pitch", val_str, NULL);
        _exit(1);
    } else if (pid > 0) {
        /* Parent: don't wait, continue audio loop.
         * Zombie will be reaped by init (PID 1). */
        pitch_last_saved = avg_pitch;
        printf("[PITCH] Saving pitch %u in background (delta=%d)\n", avg_pitch, delta);
    } else {
        fprintf(stderr, "[PITCH] WARNING: fork failed\n");
    }
}

/* Update pitch EMA from current sysfs value */
static void update_pitch_tracking(void) {
    int pitch = read_sysfs_int(SYSFS_FEEDBACK_FILE);
    if (pitch < 999000 || pitch > 1001000) return;

    /* Skip samples at saturation limits — controller hasn't converged */
    if (pitch <= 999502 || pitch >= 1000498) {
        pitch_samples = 0;  /* Reset — not converged yet */
        return;
    }

    if (pitch_samples == 0) {
        pitch_ema = (long)pitch * 256;
        pitch_samples = 1;
    } else {
        /* EMA: new = old + (val - old) / alpha */
        pitch_ema += ((long)pitch * 256 - pitch_ema) / PITCH_EMA_ALPHA;
        pitch_samples++;
    }

    /* Track min/max in current stability window */
    if (pitch < pitch_min) pitch_min = pitch;
    if (pitch > pitch_max) pitch_max = pitch;
    pitch_window_count++;

    /* Check stability at end of window */
    if (pitch_window_count >= PITCH_SETTLE_SAMPLES) {
        int range = pitch_max - pitch_min;
        if (range <= PITCH_STABLE_RANGE) {
            save_pitch();
        }
        /* Reset window */
        pitch_min = 1001000;
        pitch_max = 999000;
        pitch_window_count = 0;
    }
}

static void sighandler(int sig) {
    running = 0;
}

/* Determine if frequency is a native DSD bit rate (44.1kHz or 48kHz family) */
static int is_dsd_rate(unsigned int rate) {
    return (rate == DSD64_RATE    || rate == DSD128_RATE    ||
            rate == DSD256_RATE   || rate == DSD512_RATE    ||
            rate == DSD64_RATE_48 || rate == DSD128_RATE_48 ||
            rate == DSD256_RATE_48 || rate == DSD512_RATE_48);
}

/* Base rate of DSD64 for the frequency family of a given DSD rate */
static unsigned int dsd_base_rate(unsigned int rate) {
    return (rate % 44100 == 0) ? DSD64_RATE : DSD64_RATE_48;
}

/* Get DSD format name by frequency */
static const char* get_dsd_name(unsigned int rate) {
    switch (rate) {
        case DSD64_RATE:     return "DSD64/44.1";
        case DSD128_RATE:    return "DSD128/44.1";
        case DSD256_RATE:    return "DSD256/44.1";
        case DSD512_RATE:    return "DSD512/44.1";
        case DSD64_RATE_48:  return "DSD64/48";
        case DSD128_RATE_48: return "DSD128/48";
        case DSD256_RATE_48: return "DSD256/48";
        case DSD512_RATE_48: return "DSD512/48";
        default: return "Unknown";
    }
}

/* Initialize elastic buffer system */
static void init_elastic_buffer(void) {
    ebuf.target_size = BUFFER_TARGET_SIZE;
    ebuf.min_size = BUFFER_MIN_SIZE;
    ebuf.max_size = BUFFER_MAX_SIZE;
    ebuf.drift_trend = 0;
    ebuf.history_index = 0;
    ebuf.last_micro_check = 0;
    ebuf.last_macro_check = 0;
    ebuf.stability_counter = 0;

    for (int i = 0; i < 5; i++) {
        ebuf.drift_history[i] = 0;
    }

    printf("[ELASTIC] Initialized: target=%lu, min=%lu, max=%lu\n",
           ebuf.target_size, ebuf.min_size, ebuf.max_size);
    fflush(stdout);
}

/* Micro-adjustment: Fine-tune buffer every 5 seconds */
static void micro_adjust_buffer(snd_pcm_t *pcm_playback, snd_pcm_sframes_t delay) {
    long current_drift = delay - ebuf.target_size;

    /* Add to history */
    ebuf.drift_history[ebuf.history_index] = current_drift;
    ebuf.history_index = (ebuf.history_index + 1) % 5;

    /* Calculate trend */
    long trend_sum = 0;
    for (int i = 0; i < 5; i++) {
        trend_sum += ebuf.drift_history[i];
    }
    ebuf.drift_trend = trend_sum / 5;

    printf("[MICRO] drift=%ld, trend=%ld, delay=%ld\n",
           current_drift, ebuf.drift_trend, delay);
    fflush(stdout);
}

/* Macro-adjustment: Major buffer changes every 30 seconds */
static int macro_adjust_buffer(snd_pcm_t *pcm_playback, snd_pcm_sframes_t delay) {
    long current_drift = delay - ebuf.target_size;

    /* Emergency buffer reset if critically high */
    if (delay > BUFFER_MAX_SIZE * 0.9) {
        printf("[CRITICAL] Buffer overflow: %ld samples (EMERGENCY RESET)\n", delay);
        fflush(stdout);

        /* Emergency buffer reset without audio interruption */
        if (snd_pcm_drop(pcm_playback) == 0) {
            snd_pcm_prepare(pcm_playback);
            ebuf.stability_counter = 0;
            printf("[EMERGENCY] Buffer reset successfully\n");
            fflush(stdout);
            return 1;
        }
        return 0;
    }

    /* Adaptive buffer sizing based on drift trend */
    if (abs(ebuf.drift_trend) > DRIFT_THRESHOLD * 10) {
        if (ebuf.drift_trend > 0 && ebuf.target_size < ebuf.max_size) {
            /* Increase buffer size */
            ebuf.target_size *= 1.5;
            if (ebuf.target_size > ebuf.max_size) {
                ebuf.target_size = ebuf.max_size;
            }
            printf("[MACRO] Increasing buffer to %lu (positive drift)\n", ebuf.target_size);
            fflush(stdout);
        } else if (ebuf.drift_trend < 0 && ebuf.target_size > ebuf.min_size) {
            /* Decrease buffer size */
            ebuf.target_size *= 0.75;
            if (ebuf.target_size < ebuf.min_size) {
                ebuf.target_size = ebuf.min_size;
            }
            printf("[MACRO] Decreasing buffer to %lu (negative drift)\n", ebuf.target_size);
            fflush(stdout);
        }
        ebuf.stability_counter = 0;
    } else {
        ebuf.stability_counter++;
        if (ebuf.stability_counter >= STABILITY_THRESHOLD) {
            printf("[STABLE] Buffer stable for %d checks at size %lu\n",
                   ebuf.stability_counter, ebuf.target_size);
            fflush(stdout);
        }
    }

    return 0;
}

/* Advanced elastic buffer drift management */
static void manage_buffer_drift(snd_pcm_t *pcm_playback) {
    unsigned long current_time;
    snd_pcm_sframes_t delay;

    /* Safety checks */
    if (!pcm_playback) {
        return;
    }

    current_time = time(NULL);

    /* Get current delay safely */
    delay = snd_pcm_avail(pcm_playback);
    if (delay < 0) {
        return;
    }

    /* Micro-adjustment: Every 5 seconds */
    if (current_time - ebuf.last_micro_check >= MICRO_CHECK_INTERVAL) {
        micro_adjust_buffer(pcm_playback, delay);
        ebuf.last_micro_check = current_time;
    }

    /* Macro-adjustment: Every 30 seconds */
    if (current_time - ebuf.last_macro_check >= MACRO_CHECK_INTERVAL) {
        if (macro_adjust_buffer(pcm_playback, delay)) {
            /* Emergency reset was performed */
            printf("[RECOVERY] Emergency reset completed, continuing...\n");
            fflush(stdout);
        }
        ebuf.last_macro_check = current_time;
        last_drift_check_time = current_time;
    }

    /* Continuous monitoring for critical conditions */
    if (delay > BUFFER_MAX_SIZE * 0.8) {
        printf("[WARNING] Buffer usage high: %ld/%lu (%.1f%%)\n",
               delay, ebuf.max_size, (double)delay / ebuf.max_size * 100);
        fflush(stdout);
    }
}

/* Initialize volume sync: open mixers for UAC2 and I2S */
static int init_volume_sync(int card) {
    char uac_mixer_name[16];
    snd_mixer_selem_id_t *sid;

    sprintf(uac_mixer_name, "hw:%d", card);

    /* Open UAC2 mixer */
    if (snd_mixer_open(&uac2_mixer, 0) < 0) {
        fprintf(stderr, "[VOLUME] Cannot open UAC2 mixer\n");
        return -1;
    }
    if (snd_mixer_attach(uac2_mixer, uac_mixer_name) < 0) {
        fprintf(stderr, "[VOLUME] Cannot attach UAC2 mixer\n");
        snd_mixer_close(uac2_mixer);
        uac2_mixer = NULL;
        return -1;
    }
    if (snd_mixer_selem_register(uac2_mixer, NULL, NULL) < 0 ||
        snd_mixer_load(uac2_mixer) < 0) {
        fprintf(stderr, "[VOLUME] Cannot load UAC2 mixer\n");
        snd_mixer_close(uac2_mixer);
        uac2_mixer = NULL;
        return -1;
    }

    /* Open I2S mixer */
    if (snd_mixer_open(&i2s_mixer, 0) < 0) {
        fprintf(stderr, "[VOLUME] Cannot open I2S mixer\n");
        return -1;
    }
    if (snd_mixer_attach(i2s_mixer, "hw:0") < 0) {
        fprintf(stderr, "[VOLUME] Cannot attach I2S mixer\n");
        snd_mixer_close(i2s_mixer);
        i2s_mixer = NULL;
        return -1;
    }
    if (snd_mixer_selem_register(i2s_mixer, NULL, NULL) < 0 ||
        snd_mixer_load(i2s_mixer) < 0) {
        fprintf(stderr, "[VOLUME] Cannot load I2S mixer\n");
        snd_mixer_close(i2s_mixer);
        i2s_mixer = NULL;
        return -1;
    }

    /* Find PCM elements */
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "PCM");

    uac2_volume_elem = snd_mixer_find_selem(uac2_mixer, sid);
    i2s_volume_elem = snd_mixer_find_selem(i2s_mixer, sid);

    if (!uac2_volume_elem || !i2s_volume_elem) {
        fprintf(stderr, "[VOLUME] Cannot find PCM elements\n");
        return -1;
    }

    printf("[VOLUME] Volume sync initialized: UAC2 (hw:%d) ↔ I2S (hw:0)\n", card);
    return 0;
}

/* Synchronize volume: read UAC2, apply to I2S */
static void sync_volume(void) {
    long uac2_vol, i2s_vol;
    int uac2_mute, i2s_mute;

    if (!uac2_volume_elem || !i2s_volume_elem)
        return;

    /* Update mixer state before reading (critical!) */
    snd_mixer_handle_events(uac2_mixer);

    /* Read UAC2 volume and mute */
    snd_mixer_selem_get_capture_volume(uac2_volume_elem, SND_MIXER_SCHN_MONO, &uac2_vol);
    snd_mixer_selem_get_capture_switch(uac2_volume_elem, SND_MIXER_SCHN_MONO, &uac2_mute);

    /* Only if changed (check BOTH volume AND mute) */
    if (uac2_vol != last_uac2_volume || uac2_mute != last_uac2_mute) {
        /* Apply to I2S */
        snd_mixer_selem_set_playback_volume_all(i2s_volume_elem, uac2_vol);
        snd_mixer_selem_set_playback_switch_all(i2s_volume_elem, uac2_mute);

        last_uac2_volume = uac2_vol;
        last_uac2_mute = uac2_mute;
        printf("[VOLUME] Synced: %ld%% %s\n", uac2_vol, uac2_mute ? "ON" : "MUTE");
    }
}

/* Close mixers */
static void close_mixers(void) {
    if (uac2_mixer) {
        snd_mixer_close(uac2_mixer);
        uac2_mixer = NULL;
    }
    if (i2s_mixer) {
        snd_mixer_close(i2s_mixer);
        i2s_mixer = NULL;
    }
}

/* Find UAC card in /sys/class/u_audio/ */
static int find_uac_card(void) {
    char path[256];
    struct stat st;

    /* Try uac_card0, uac_card1, etc */
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "%s/uac_card%d", SYSFS_UAC2_PATH, i);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(uac_card_path, path, sizeof(uac_card_path) - 1);
            snprintf(uac_card_name, sizeof(uac_card_name), "uac_card%d", i);
            printf("Found UAC card: %s (card %d)\n", uac_card_path, i);
            return i;  // Return card number
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

/* Configure PCM device */
static int setup_pcm(snd_pcm_t **pcm, const char *device, snd_pcm_stream_t stream,
                     unsigned int rate, snd_pcm_format_t format, unsigned int channels)
{
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    int err;
    /* Adaptive period size: larger for high frequencies (DSD) ~0.7ms per period.
     * Uses the base rate of each DSD family as the multiplier reference:
     *   44.1k: DSD64=2048 DSD128=4096 DSD256=8192 DSD512=16384 frames
     *   48k:   DSD64=2048 DSD128=4096 DSD256=8192 DSD512=16384 frames */
    snd_pcm_uframes_t period_size = PERIOD_FRAMES;
    if (is_dsd_rate(rate)) {
        period_size = (rate / dsd_base_rate(rate)) * 2048;
    }
    /* Capture: small period + buffer so kernel PI feedback target (buffer/2)
     * is reachable.  Playback: large period + buffer for XRUN protection. */
    if (stream == SND_PCM_STREAM_CAPTURE) {
        period_size = 512;  /* Small period for capture — PI needs low target */
    }
    snd_pcm_uframes_t buffer_size = period_size * 16;

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

/* Configure audio with given frequency */
static int configure_audio(unsigned int rate, int card, char **buffer, size_t *buffer_size,
                          snd_pcm_uframes_t *period_size_out) {
    snd_pcm_uframes_t capture_period_size, playback_period_size;
    int is_dsd = is_dsd_rate(rate);
    snd_pcm_format_t i2s_format = is_dsd ? I2S_FORMAT_DSD : I2S_FORMAT_PCM;

    if (is_dsd) {
        printf("\n[CONFIG] ═══ DSD MODE: %s (%u Hz) ═══\n", get_dsd_name(rate), rate);
        is_current_dsd = 1;
        dsd_dump_done = 0;
        /* dsd_sample_swap=0 is set globally in usb_to_i2s.sh —
         * USB RAW_DATA DSD already arrives in DAC-native bit order. */
    } else {
        printf("\n[CONFIG] Setting up PCM audio: %u Hz, 32-bit, Stereo\n", rate);
        is_current_dsd = 0;
    }

    /* Initialize elastic buffer system */
    init_elastic_buffer();

    /* Reset I2S start flag */
    i2s_started = 0;

    close_pcms();

    /* UAC2 capture - ALWAYS use PCM S32_LE format
     * UAC2 gadget RAW_DATA (Alt Setting 2) transmits DSD as raw 32-bit data at the
     * LRCK rate (bit_rate/32). ALSA sees it as PCM. We route to I2S as DSD.
     *
     * Both UAC2 and I2S operate at the LRCK rate (rate/32 for DSD).
     * Data flow is 1:1 in frames — UAC2 produces DSD bits packed as S32_LE,
     * I2S consumes the same bits as DSD_U32_LE. */
    char uac_device[32];
    sprintf(uac_device, "hw:%d,0", card);
    unsigned int lrck_rate = is_dsd ? rate / 32 : rate;  /* LRCK rate for both devices */
    if (setup_pcm(&pcm_capture, uac_device, SND_PCM_STREAM_CAPTURE,
                  lrck_rate, I2S_FORMAT_PCM, I2S_CHANNELS) < 0) {
        return -1;
    }

    /* I2S playback - both PCM and DSD use LRCK rate; format differs */
    if (setup_pcm(&pcm_playback, I2S_CARD, SND_PCM_STREAM_PLAYBACK,
                  lrck_rate, i2s_format, I2S_CHANNELS) < 0) {
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
    *buffer = realloc(*buffer, *buffer_size);

    if (!*buffer) {
        fprintf(stderr, "Cannot allocate buffer\n");
        close_pcms();
        return -1;
    }

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
    int uevent_sock = -1;
    char uevent_buf[UEVENT_BUFFER_SIZE];
    int uac_card = -1;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Real-time scheduling: prevent buffer overruns from scheduler latency.
     * DSD is extremely sensitive — even 5ms delay corrupts the bitstream.
     * SCHED_FIFO priority 70 (below IRQ threads ~50, above normal audio ~60). */
    {
        struct sched_param sp = { .sched_priority = 70 };
        if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
            printf("[RT] SCHED_FIFO priority %d set\n", sp.sched_priority);
        else
            fprintf(stderr, "[RT] WARNING: Cannot set SCHED_FIFO: %s (running as normal priority)\n",
                    strerror(errno));
    }

    /* Lock all pages in memory — prevent page faults during audio routing */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
        printf("[RT] Memory locked (no page faults)\n");

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  UAC2 -> I2S Router (uevent-based, PureCore compatible)\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Find UAC card */
    uac_card = find_uac_card();
    if (uac_card < 0) {
        fprintf(stderr, "ERROR: UAC2 device not found. Is gadget loaded?\n");
        return 1;
    }

    /* Read static parameters (format and channels) */
    int format_bytes = read_sysfs_int(SYSFS_FORMAT_FILE);
    int channels = read_sysfs_int(SYSFS_CHANNELS_FILE);

    if (format_bytes < 0 || channels < 0) {
        fprintf(stderr, "ERROR: Cannot read UAC2 configuration\n");
        return 1;
    }

    printf("UAC2 Configuration (static):\n");
    printf("  Format:   %d bytes (%d-bit)\n", format_bytes, format_bytes * 8);
    printf("  Channels: %d\n\n", channels);

    if (format_bytes != 4) {
        printf("WARNING: UAC2 format is not 32-bit. Recommended:\n");
        printf("  echo 4 > /sys/kernel/config/usb_gadget/xingcore/functions/uac2.0/c_ssize\n\n");
    }

    /* Create netlink socket for uevent */
    uevent_sock = create_uevent_socket();
    if (uevent_sock < 0) {
        fprintf(stderr, "ERROR: Cannot create uevent socket\n");
        return 1;
    }

    printf("Listening for kobject uevent from u_audio driver...\n");
    printf("Waiting for rate changes...\n\n");

    /* Load saved feedback pitch from U-Boot env */
    load_saved_pitch();

    /* Read initial frequency and configure audio */
    int rate = read_sysfs_int(SYSFS_RATE_FILE);
    if (rate > 0) {
        printf("Initial rate: %d Hz\n", rate);
        if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
            current_rate = rate;
        }
    }

    /* Volume sync disabled - UAC2 has no mixer controls */
    printf("[VOLUME] Volume sync disabled - UAC2 has no mixer controls\n");

    /* Main loop */
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
                    printf("\n[CHANGE] Rate changed: %u Hz -> %u Hz\n", current_rate, rate);

                    if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
                        current_rate = rate;
                    }
                }
            }
        }

        /* Drift compensation handled by kernel PI feedback controller.
         * manage_buffer_drift disabled — it conflicts with kernel feedback
         * and incorrectly interprets snd_pcm_avail (free space vs fill). */

        /* Pitch learning: periodically track feedback for save/restore */
        pitch_loop_counter++;
        if (pcm_capture && (pitch_loop_counter % PITCH_READ_INTERVAL) == 0) {
            update_pitch_tracking();
        }

        /* Audio routing */
        if (!pcm_capture || !pcm_playback) {
            usleep(100000);  /* 100ms */
            continue;
        }

        /* Wait for data to be available (reduces CPU load) */
        int err = snd_pcm_wait(pcm_capture, 100);  /* Timeout 100ms */
        if (err <= 0) {
            if (err == 0) {
                /* Timeout - this is normal, continue */
                consecutive_errors = 0;  /* Reset counter */
                continue;
            }
            /* Error - increment counter */
            consecutive_errors++;

            /* Too many consecutive errors - reopen PCM */
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                fprintf(stderr, "[ERROR] Too many consecutive errors (%d), reopening PCM devices...\n",
                        consecutive_errors);
                close_pcms();
                consecutive_errors = 0;
                usleep(500000);  /* 500ms before reopening */
                continue;
            }

            /* Error handling */
            if (err == -EPIPE) {
                /* XRUN - try to recover */
                fprintf(stderr, "[WARN] snd_pcm_wait: XRUN, recovering... (error #%d)\n", consecutive_errors);
                snd_pcm_prepare(pcm_capture);
                snd_pcm_start(pcm_capture);
                snd_pcm_prepare(pcm_playback);
                i2s_started = 0;  /* Will restart on next successful write */

                /* Reset I2S auto-mute after recovery */
                if (system("echo 0 > /sys/devices/platform/ffae0000.i2s/mute 2>/dev/null") == 0) {
                    fprintf(stderr, "[INFO] I2S auto-mute reset after XRUN recovery\n");
                }
            } else {
                /* Other error - log and delay */
                fprintf(stderr, "[ERROR] snd_pcm_wait failed: %s (%d) (error #%d)\n",
                        snd_strerror(err), err, consecutive_errors);
                usleep(10000);  /* 10ms - prevents tight loop */
            }
            continue;
        }

        /* Successfully received data - reset error counter */
        consecutive_errors = 0;

        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, buffer, period_size);

        if (frames < 0) {
            consecutive_errors++;

            /* Protection from infinite read errors */
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                fprintf(stderr, "[ERROR] Too many read errors (%d), reopening PCM...\n", consecutive_errors);
                close_pcms();
                consecutive_errors = 0;
                usleep(500000);
                continue;
            }

            if (frames == -EPIPE) {
                fprintf(stderr, "[XRUN] Capture overrun (error #%d)\n", consecutive_errors);
                snd_pcm_prepare(pcm_capture);
                snd_pcm_start(pcm_capture);
                snd_pcm_prepare(pcm_playback);
                i2s_started = 0;  /* Will restart on next successful write */

                /* Reset I2S auto-mute after XRUN recovery */
                if (system("echo 0 > /sys/devices/platform/ffae0000.i2s/mute 2>/dev/null") == 0) {
                    fprintf(stderr, "[INFO] I2S auto-mute reset after capture XRUN\n");
                }
                continue;
            } else if (frames == -ESTRPIPE) {
                while ((frames = snd_pcm_resume(pcm_capture)) == -EAGAIN)
                    usleep(10000);
                if (frames < 0)
                    snd_pcm_prepare(pcm_capture);
                continue;
            } else if (frames == -ENODEV || frames == -EBADF) {
                fprintf(stderr, "[ERROR] Stream disconnected, waiting... (error #%d)\n", consecutive_errors);
                close_pcms();
                consecutive_errors = 0;
                usleep(500000);  /* 500ms */
                continue;
            }
            fprintf(stderr, "[ERROR] Read failed: %s (error #%d)\n", snd_strerror(frames), consecutive_errors);
            usleep(10000);
            continue;
        }

        if (frames > 0) {
            /* Apply saved pitch now that stream is running */
            apply_saved_pitch();

            /* DSD data dump: save first 8KB + hex print first 4 frames */
            if (is_current_dsd && !dsd_dump_done) {
                FILE *df = fopen("/tmp/dsd_usb_dump.raw", "wb");
                if (df) {
                    size_t dump_bytes = frames * I2S_CHANNELS * 4;
                    if (dump_bytes > 8192) dump_bytes = 8192;
                    fwrite(buffer, 1, dump_bytes, df);
                    fclose(df);
                    printf("[DSD DUMP] Saved %zu bytes to /tmp/dsd_usb_dump.raw\n", dump_bytes);
                }
                /* Print first 4 frames (4 × 2ch × 4bytes = 32 bytes) as hex */
                unsigned char *b = (unsigned char *)buffer;
                int dump_frames = (frames < 4) ? frames : 4;
                printf("[DSD DUMP] First %d frames (L=4bytes R=4bytes per frame):\n", dump_frames);
                for (int f = 0; f < dump_frames; f++) {
                    int off = f * I2S_CHANNELS * 4;
                    printf("  Frame %d: L=[%02x %02x %02x %02x] R=[%02x %02x %02x %02x]\n",
                           f, b[off], b[off+1], b[off+2], b[off+3],
                           b[off+4], b[off+5], b[off+6], b[off+7]);
                }
                fflush(stdout);
                dsd_dump_done = 1;
            }

            /* DSD byte-swap: USB RAW_DATA byte order [oldest..newest] differs
             * from ALSA DSD_U32_LE [newest..oldest]. Without bswap, VDW=16
             * I2S sends 8-bit groups in wrong temporal order → violet noise. */
            if (is_current_dsd) {
                uint32_t *w = (uint32_t *)buffer;
                size_t nwords = frames * I2S_CHANNELS;
                for (size_t i = 0; i < nwords; i++)
                    w[i] = BSWAP32(w[i]);
            }

            snd_pcm_sframes_t written = snd_pcm_writei(pcm_playback, buffer, frames);

            /* Playback auto-starts via start_threshold (buffer/2).
             * No manual snd_pcm_start — avoids underrun with large buffers. */
            if (written > 0 && !i2s_started) {
                i2s_started = 1;
                printf("[START] I2S playback buffering (auto-start at threshold)\n");
                fflush(stdout);
            }

            if (written < 0) {
                if (written == -EPIPE) {
                    fprintf(stderr, "[XRUN] Playback underrun\n");
                    snd_pcm_prepare(pcm_playback);
                    i2s_started = 0;  /* Will restart on next successful write */
                } else if (written == -ESTRPIPE) {
                    while ((written = snd_pcm_resume(pcm_playback)) == -EAGAIN)
                        usleep(10000);
                    if (written < 0)
                        snd_pcm_prepare(pcm_playback);
                } else if (written == -ENODEV || written == -EBADF) {
                    printf("[ERROR] Playback error, reinitializing...\n");
                    i2s_started = 0;  /* Reset I2S start flag */
                    close_pcms();
                    usleep(500000);
                }
                continue;
            }
        }

        /* Volume sync disabled */
    }

    /* Cleanup */
    if (uevent_sock >= 0)
        close(uevent_sock);

    if (buffer)
        free(buffer);

    close_pcms();
    /* close_mixers() disabled - not used */

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  UAC2 -> I2S Router stopped\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}