/*
 * Simple USB UAC2 to I2S Audio Router - No rate control
 * Just route whatever comes from USB to I2S
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <errno.h>

#define PERIOD_SIZE 2048
#define BUFFER_SIZE (PERIOD_SIZE * 4)

typedef struct {
    snd_pcm_t *capture_handle;
    snd_pcm_t *playback_handle;
    int running;
    pthread_t thread;
} audio_router_t;

static audio_router_t router = {0};

void signal_handler(int sig) {
    printf("\nShutting down...\n");
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

/* Simple PCM configuration - detect actual rate */
int configure_pcm_simple(snd_pcm_t *handle, int is_capture) {
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

    if ((err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S32_LE)) < 0) {
        fprintf(stderr, "Cannot set format: %s\n", snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params_set_channels(handle, hw_params, 2)) < 0) {
        fprintf(stderr, "Cannot set channel count: %s\n", snd_strerror(err));
        return err;
    }

    /* Try to set a high rate and let ALSA negotiate the actual one */
    rate = 192000;
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

    /* Get actual rate */
    snd_pcm_hw_params_get_rate(hw_params, &rate, 0);
    printf("%s: Actual rate = %u Hz\n", is_capture ? "Capture" : "Playback", rate);

    /* Software parameters */
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

void* audio_thread(void *arg) {
    char *buffer;
    snd_pcm_sframes_t frames;
    int buffer_size = PERIOD_SIZE * 2 * 4; // stereo * 32-bit

    buffer = malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Cannot allocate audio buffer\n");
        return NULL;
    }

    printf("Audio routing started\n");

    /* Start UAC2 capture stream */
    if (snd_pcm_start(router.capture_handle) < 0) {
        fprintf(stderr, "Cannot start UAC2 capture stream\n");
        free(buffer);
        return NULL;
    }

    /* Start I2S playback stream */
    if (snd_pcm_start(router.playback_handle) < 0) {
        fprintf(stderr, "Cannot start I2S playback stream\n");
        free(buffer);
        return NULL;
    }

    while (router.running) {
        /* Read from UAC2 playback stream (Windows->USB) */
        frames = snd_pcm_readi(router.capture_handle, buffer, PERIOD_SIZE);

        if (frames < 0) {
            if (frames == -EPIPE) {
                /* Overrun */
                snd_pcm_prepare(router.capture_handle);
                snd_pcm_start(router.capture_handle);
                continue;
            } else {
                fprintf(stderr, "Read error: %s\n", snd_strerror(frames));
                usleep(10000);
                continue;
            }
        }

        if (frames == 0)
            continue;

        /* Write to I2S */
        snd_pcm_sframes_t written = snd_pcm_writei(router.playback_handle, buffer, frames);

        if (written < 0) {
            if (written == -EPIPE) {
                fprintf(stderr, "Playback underrun, recovering...\n");
                snd_pcm_prepare(router.playback_handle);
                continue;
            } else {
                fprintf(stderr, "Write error: %s\n", snd_strerror(written));
                usleep(10000);
                continue;
            }
        }
    }

    free(buffer);
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

    /* Open UAC2 capture device (Windows sends audio TO capture) */
    snprintf(pcm_name, sizeof(pcm_name), "hw:%d,0", uac2_card);
    if ((err = snd_pcm_open(&router.capture_handle, pcm_name, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "Cannot open UAC2 capture device %s: %s\n", pcm_name, snd_strerror(err));
        return err;
    }

    /* Open playback */
    snprintf(pcm_name, sizeof(pcm_name), "hw:%d,0", i2s_card);
    if ((err = snd_pcm_open(&router.playback_handle, pcm_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "Cannot open playback device %s: %s\n", pcm_name, snd_strerror(err));
        snd_pcm_close(router.capture_handle);
        return err;
    }

    /* Configure capture */
    if (configure_pcm_simple(router.capture_handle, 1) < 0) {
        snd_pcm_close(router.capture_handle);
        snd_pcm_close(router.playback_handle);
        return -1;
    }

    /* Configure playback */
    if (configure_pcm_simple(router.playback_handle, 0) < 0) {
        snd_pcm_close(router.capture_handle);
        snd_pcm_close(router.playback_handle);
        return -1;
    }

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
}

int main(int argc, char *argv[]) {
    printf("Simple USB UAC2 to I2S Audio Router\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (start_routing() < 0) {
        fprintf(stderr, "Failed to start audio routing\n");
        return 1;
    }

    printf("Press Ctrl+C to stop...\n");

    while (router.running) {
        sleep(1);
    }

    stop_routing();
    printf("Router stopped\n");

    return 0;
}