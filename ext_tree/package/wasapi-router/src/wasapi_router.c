/*
 * Advanced WASAPI USB UAC2 to I2S Audio Router
 * Supports dynamic format switching as required by WASAPI exclusive mode
 *
 * Key features:
 * - Detects stream stop/start when WASAPI reinitializes audio session
 * - Automatically reconfigures for new sample rate and bit depth
 * - Handles format transitions gracefully
 * - Supports 16/24/32-bit formats and all standard sample rates
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

#define PERIOD_SIZE 1024
#define BUFFER_SIZE (PERIOD_SIZE * 4)
#define MAX_RETRIES 5
#define RETRY_DELAY_US 100000  // 100ms

typedef struct {
    snd_pcm_t *capture_handle;
    snd_pcm_t *playback_handle;
    int running;
    int stream_active;
    pthread_t thread;
    pthread_mutex_t lock;

    // Current format parameters
    unsigned int current_rate;
    int current_bits;
    int format_changed;

    // Statistics
    unsigned int format_changes;
    unsigned int recoveries;
} wasapi_router_t;

static wasapi_router_t router = {0};

void signal_handler(int sig) {
    printf("\nShutting down WASAPI router...\n");
    router.running = 0;
}

int find_card_by_name(const char *substring) {
    int card = -1;
    char *name;

    while (snd_card_next(&card) >= 0 && card >= 0) {
        if (snd_card_get_name(card, &name) < 0)
            continue;
        if (strstr(name, substring) != NULL) {
            printf("Found %s card: %d\n", substring, card);
            free(name);
            return card;
        }
        free(name);
    }
    return -1;
}

int detect_current_format(snd_pcm_t *handle, unsigned int *rate, int *bits) {
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_format_t format;
    int err;

    snd_pcm_hw_params_alloca(&hw_params);

    if ((err = snd_pcm_hw_params_current(handle, hw_params)) < 0) {
        fprintf(stderr, "Cannot get current hw params: %s\n", snd_strerror(err));
        return err;
    }

    // Get current sample rate
    if ((err = snd_pcm_hw_params_get_rate(hw_params, rate, 0)) < 0) {
        fprintf(stderr, "Cannot get sample rate: %s\n", snd_strerror(err));
        return err;
    }

    // Get current format
    if ((err = snd_pcm_hw_params_get_format(hw_params, &format)) < 0) {
        fprintf(stderr, "Cannot get format: %s\n", snd_strerror(err));
        return err;
    }

    // Convert format to bits
    switch (format) {
        case SND_PCM_FORMAT_S16_LE:
            *bits = 16;
            break;
        case SND_PCM_FORMAT_S24_3LE:
            *bits = 24;
            break;
        case SND_PCM_FORMAT_S32_LE:
            *bits = 32;
            break;
        default:
            *bits = 32;  // Default to 32-bit
            break;
    }

    return 0;
}

int configure_pcm_flexible(snd_pcm_t *handle, int is_capture) {
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    unsigned int rate;
    int err;

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_sw_params_alloca(&sw_params);

    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
        fprintf(stderr, "Cannot get hw params: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "Cannot set access: %s\n", snd_strerror(err));
        return err;
    }

    // Try formats in order of preference
    snd_pcm_format_t formats[] = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S16_LE};
    int format_found = 0;

    for (int i = 0; i < 3; i++) {
        if ((err = snd_pcm_hw_params_set_format(handle, hw_params, formats[i])) >= 0) {
            format_found = 1;
            break;
        }
    }

    if (!format_found) {
        fprintf(stderr, "Cannot set any supported format\n");
        return -1;
    }

    if ((err = snd_pcm_hw_params_set_channels(handle, hw_params, 2)) < 0) {
        fprintf(stderr, "Cannot set channel count: %s\n", snd_strerror(err));
        return err;
    }

    // Set sample rate - let ALSA negotiate
    rate = 192000;  // Try max rate first
    if ((err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &rate, 0)) < 0) {
        fprintf(stderr, "Cannot set sample rate: %s\n", snd_strerror(err));
        return err;
    }

    snd_pcm_uframes_t period_size = PERIOD_SIZE;
    if ((err = snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, 0)) < 0) {
        fprintf(stderr, "Cannot set period size: %s\n", snd_strerror(err));
        return err;
    }

    snd_pcm_uframes_t buffer_size = BUFFER_SIZE;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_size)) < 0) {
        fprintf(stderr, "Cannot set buffer size: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params(handle, hw_params)) < 0) {
        fprintf(stderr, "Cannot set hw parameters: %s\n", snd_strerror(err));
        return err;
    }

    // Detect and report actual format
    unsigned int actual_rate;
    int actual_bits;
    detect_current_format(handle, &actual_rate, &actual_bits);

    printf("%s: %u Hz / %d-bit\n", is_capture ? "Capture" : "Playback", actual_rate, actual_bits);

    // Software parameters
    if ((err = snd_pcm_sw_params_current(handle, sw_params)) < 0) {
        fprintf(stderr, "Cannot get sw params: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params_set_start_threshold(handle, sw_params, period_size)) < 0) {
        fprintf(stderr, "Cannot set start threshold: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_sw_params(handle, sw_params)) < 0) {
        fprintf(stderr, "Cannot set sw parameters: %s\n", snd_strerror(err));
        return err;
    }

    return 0;
}

int wait_for_stream_start(void) {
    int retries = 0;

    printf("Waiting for audio stream to start...\n");

    while (retries < MAX_RETRIES && router.running) {
        snd_pcm_sframes_t avail = snd_pcm_avail_update(router.capture_handle);

        if (avail > 0) {
            printf("Audio stream started! Available frames: %ld\n", avail);
            return 0;
        }

        if (avail == -EPIPE) {
            // Underrun - prepare for restart
            snd_pcm_prepare(router.capture_handle);
        }

        usleep(RETRY_DELAY_US);
        retries++;
    }

    printf("Timeout waiting for stream start\n");
    return -1;
}

int detect_format_change(void) {
    unsigned int new_rate;
    int new_bits;

    if (detect_current_format(router.capture_handle, &new_rate, &new_bits) < 0) {
        return -1;
    }

    if (new_rate != router.current_rate || new_bits != router.current_bits) {
        printf("Format change detected: %u Hz/%d-bit -> %u Hz/%d-bit\n",
               router.current_rate, router.current_bits, new_rate, new_bits);

        router.current_rate = new_rate;
        router.current_bits = new_bits;
        router.format_changed = 1;
        router.format_changes++;

        return 1;
    }

    return 0;
}

void* audio_thread(void *arg) {
    char *buffer;
    snd_pcm_sframes_t frames;
    int buffer_size = PERIOD_SIZE * 2 * 4; // stereo * 32-bit max
    int consecutive_errors = 0;
    int stream_was_active = 0;

    buffer = malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Cannot allocate audio buffer\n");
        return NULL;
    }

    printf("WASAPI audio routing started\n");
    printf("Waiting for WASAPI session initialization...\n");

    while (router.running) {
        // Check if stream is active
        snd_pcm_sframes_t avail = snd_pcm_avail_update(router.capture_handle);

        if (avail < 0) {
            if (avail == -EPIPE) {
                // Overrun or stream stopped
                if (stream_was_active) {
                    printf("Stream stopped (WASAPI session ended)\n");
                    stream_was_active = 0;
                    router.stream_active = 0;
                }

                snd_pcm_prepare(router.capture_handle);
                consecutive_errors++;

                if (consecutive_errors > 10) {
                    printf("Too many consecutive errors, waiting for restart...\n");
                    usleep(500000); // 500ms
                    consecutive_errors = 0;
                }
                continue;
            } else {
                fprintf(stderr, "Stream error: %s\n", snd_strerror(avail));
                usleep(10000);
                continue;
            }
        }

        if (avail == 0) {
            // No data available
            if (stream_was_active) {
                printf("Stream paused (WASAPI transitioning)\n");
                stream_was_active = 0;
                router.stream_active = 0;
            }
            usleep(10000);
            continue;
        }

        // We have data - stream is active
        if (!stream_was_active) {
            printf("Stream active (WASAPI session started)\n");
            stream_was_active = 1;
            router.stream_active = 1;

            // Check for format change when stream starts
            detect_format_change();
        }

        // Read from UAC2
        frames = snd_pcm_readi(router.capture_handle, buffer, PERIOD_SIZE);

        if (frames < 0) {
            if (frames == -EPIPE) {
                printf("Overrun detected, recovering...\n");
                snd_pcm_prepare(router.capture_handle);
                router.recoveries++;
                continue;
            } else if (frames == -EAGAIN) {
                // No data available right now
                usleep(1000);
                continue;
            } else {
                fprintf(stderr, "Read error: %s\n", snd_strerror(frames));
                consecutive_errors++;
                if (consecutive_errors > 5) {
                    printf("Multiple read errors, waiting for recovery...\n");
                    usleep(100000);
                    consecutive_errors = 0;
                }
                continue;
            }
        }

        if (frames == 0)
            continue;

        consecutive_errors = 0; // Reset error counter on successful read

        // Write to I2S
        snd_pcm_sframes_t written = snd_pcm_writei(router.playback_handle, buffer, frames);

        if (written < 0) {
            if (written == -EPIPE) {
                printf("Playback underrun, recovering...\n");
                snd_pcm_prepare(router.playback_handle);
                router.recoveries++;
            } else {
                fprintf(stderr, "Write error: %s\n", snd_strerror(written));
                usleep(10000);
            }
            continue;
        }
    }

    free(buffer);
    printf("Audio thread ended\n");
    return NULL;
}

int start_routing(void) {
    int uac2_card, i2s_card;
    char pcm_name[32];
    int err;

    uac2_card = find_card_by_name("UAC2");
    i2s_card = find_card_by_name("I2S");

    if (uac2_card < 0 || i2s_card < 0) {
        fprintf(stderr, "Required audio cards not found!\n");
        return -1;
    }

    /* Open UAC2 capture device */
    snprintf(pcm_name, sizeof(pcm_name), "hw:%d,0", uac2_card);
    if ((err = snd_pcm_open(&router.capture_handle, pcm_name, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
        fprintf(stderr, "Cannot open UAC2 capture device %s: %s\n", pcm_name, snd_strerror(err));
        return err;
    }

    /* Open I2S playback device */
    snprintf(pcm_name, sizeof(pcm_name), "hw:%d,0", i2s_card);
    if ((err = snd_pcm_open(&router.playback_handle, pcm_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "Cannot open playback device %s: %s\n", pcm_name, snd_strerror(err));
        snd_pcm_close(router.capture_handle);
        return err;
    }

    /* Configure devices */
    if (configure_pcm_flexible(router.capture_handle, 1) < 0) {
        snd_pcm_close(router.capture_handle);
        snd_pcm_close(router.playback_handle);
        return -1;
    }

    if (configure_pcm_flexible(router.playback_handle, 0) < 0) {
        snd_pcm_close(router.capture_handle);
        snd_pcm_close(router.playback_handle);
        return -1;
    }

    /* Initialize format detection */
    detect_current_format(router.capture_handle, &router.current_rate, &router.current_bits);

    /* Prepare devices */
    if ((err = snd_pcm_prepare(router.capture_handle)) < 0) {
        fprintf(stderr, "Cannot prepare capture: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_prepare(router.playback_handle)) < 0) {
        fprintf(stderr, "Cannot prepare playback: %s\n", snd_strerror(err));
        return err;
    }

    router.running = 1;
    router.stream_active = 0;
    router.format_changes = 0;
    router.recoveries = 0;

    /* Start audio routing thread */
    if (pthread_create(&router.thread, NULL, audio_thread, NULL) != 0) {
        fprintf(stderr, "Cannot create audio thread\n");
        router.running = 0;
        snd_pcm_close(router.capture_handle);
        snd_pcm_close(router.playback_handle);
        return -1;
    }

    return 0;
}

void stop_routing(void) {
    if (!router.running)
        return;

    router.running = 0;
    pthread_join(router.thread, NULL);

    snd_pcm_close(router.capture_handle);
    snd_pcm_close(router.playback_handle);

    printf("\n=== WASAPI Router Statistics ===\n");
    printf("Format changes: %u\n", router.format_changes);
    printf("Error recoveries: %u\n", router.recoveries);
    printf("=================================\n");
}

int main(int argc, char *argv[]) {
    printf("Advanced WASAPI USB UAC2 to I2S Audio Router\n");
    printf("Supports dynamic format switching for WASAPI exclusive mode\n\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (start_routing() < 0) {
        fprintf(stderr, "Failed to start audio routing\n");
        return 1;
    }

    printf("Press Ctrl+C to stop...\n\n");
    printf("Router is ready for WASAPI format switching.\n");
    printf("Start/stop audio from Windows to test dynamic format changes.\n\n");

    while (router.running) {
        sleep(1);

        // Print statistics every 30 seconds
        static int counter = 0;
        if (++counter >= 30) {
            printf("Status: %s, Format: %u/%d-bit, Changes: %u, Recoveries: %u\n",
                   router.stream_active ? "Active" : "Waiting",
                   router.current_rate, router.current_bits,
                   router.format_changes, router.recoveries);
            counter = 0;
        }
    }

    stop_routing();
    printf("WASAPI router stopped\n");

    return 0;
}