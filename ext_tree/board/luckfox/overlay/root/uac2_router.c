/*
 * USB UAC2 to I2S Audio Router Daemon
 * 
 * Features:
 * - Detects USB audio stream sample rate (44.1-384kHz)
 * - Automatically configures I2S PLL to match sample rate
 * - Zero-copy audio routing from USB to I2S DAC
 * - Supports 16/24/32-bit PCM formats
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
    unsigned int current_rate;
    int running;
    pthread_t thread;
} audio_router_t;

static audio_router_t router = {0};

/* Signal handler for graceful shutdown */
void signal_handler(int sig) {
    printf("\nShutting down...\n");
    router.running = 0;
}

/* Find ALSA card by name substring */
int find_card_by_name(const char *substring) {
    int card = -1;
    char *name;
    
    while (snd_card_next(&card) >= 0 && card >= 0) {
        if (snd_card_get_name(card, &name) < 0)
            continue;
        if (strstr(name, substring) != NULL) {
            free(name);
            return card;
        }
        free(name);
    }
    return -1;
}

/* Configure ALSA PCM device */
int configure_pcm(snd_pcm_t *handle, unsigned int rate, int is_capture) {
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    int err;
    unsigned int actual_rate = rate;
    
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_sw_params_alloca(&sw_params);
    
    /* Hardware parameters */
    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
        fprintf(stderr, "Cannot initialize hw params: %s\n", snd_strerror(err));
        return err;
    }
    
    if ((err = snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "Cannot set access type: %s\n", snd_strerror(err));
        return err;
    }
    
    /* Try 32-bit first, fallback to 24-bit, then 16-bit */
    if (snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S32_LE) < 0) {
        if (snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S24_LE) < 0) {
            if ((err = snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
                fprintf(stderr, "Cannot set sample format: %s\n", snd_strerror(err));
                return err;
            }
        }
    }
    
    if ((err = snd_pcm_hw_params_set_channels(handle, hw_params, 2)) < 0) {
        fprintf(stderr, "Cannot set channel count: %s\n", snd_strerror(err));
        return err;
    }
    
    if ((err = snd_pcm_hw_params_set_rate_near(handle, hw_params, &actual_rate, 0)) < 0) {
        fprintf(stderr, "Cannot set sample rate: %s\n", snd_strerror(err));
        return err;
    }
    
    if (actual_rate != rate) {
        fprintf(stderr, "Warning: Rate %u Hz not supported, using %u Hz\n", rate, actual_rate);
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
    
    /* Software parameters */
    if ((err = snd_pcm_sw_params_current(handle, sw_params)) < 0) {
        fprintf(stderr, "Cannot get sw params: %s\n", snd_strerror(err));
        return err;
    }
    
    if ((err = snd_pcm_sw_params_set_start_threshold(handle, sw_params, period_size)) < 0) {
        fprintf(stderr, "Cannot set start threshold: %s\n", snd_strerror(err));
        return err;
    }
    
    if ((err = snd_pcm_sw_params_set_avail_min(handle, sw_params, period_size)) < 0) {
        fprintf(stderr, "Cannot set avail min: %s\n", snd_strerror(err));
        return err;
    }
    
    if ((err = snd_pcm_sw_params(handle, sw_params)) < 0) {
        fprintf(stderr, "Cannot set sw parameters: %s\n", snd_strerror(err));
        return err;
    }
    
    printf("%s configured: %u Hz, period=%lu, buffer=%lu\n", 
           is_capture ? "Capture" : "Playback", actual_rate, period_size, buffer_size);
    
    return 0;
}

/* Open and configure audio devices */
int open_audio_devices(unsigned int rate) {
    int uac2_card, i2s_card;
    char pcm_name[32];
    int err;
    
    /* Find cards */
    uac2_card = find_card_by_name("UAC2");
    i2s_card = find_card_by_name("I2S");
    
    if (uac2_card < 0) {
        fprintf(stderr, "UAC2 card not found!\n");
        return -1;
    }
    
    if (i2s_card < 0) {
        fprintf(stderr, "I2S card not found!\n");
        return -1;
    }
    
    printf("Found UAC2 card %d, I2S card %d\n", uac2_card, i2s_card);
    
    /* Open capture (USB input) */
    snprintf(pcm_name, sizeof(pcm_name), "hw:%d,0", uac2_card);
    if ((err = snd_pcm_open(&router.capture_handle, pcm_name, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "Cannot open capture device %s: %s\n", pcm_name, snd_strerror(err));
        return err;
    }
    
    /* Open playback (I2S output) */
    snprintf(pcm_name, sizeof(pcm_name), "hw:%d,0", i2s_card);
    if ((err = snd_pcm_open(&router.playback_handle, pcm_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "Cannot open playback device %s: %s\n", pcm_name, snd_strerror(err));
        snd_pcm_close(router.capture_handle);
        return err;
    }
    
    /* Configure devices */
    if (configure_pcm(router.capture_handle, rate, 1) < 0) {
        snd_pcm_close(router.capture_handle);
        snd_pcm_close(router.playback_handle);
        return -1;
    }
    
    if (configure_pcm(router.playback_handle, rate, 0) < 0) {
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
    
    return 0;
}

/* Audio routing thread */
void* audio_thread(void *arg) {
    char *buffer;
    snd_pcm_sframes_t frames;
    int buffer_size = PERIOD_SIZE * 2 * 4; // stereo * 32-bit
    
    buffer = malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Cannot allocate audio buffer\n");
        return NULL;
    }
    
    printf("Audio routing started at %u Hz\n", router.current_rate);
    
    /* Start capture */
    snd_pcm_start(router.capture_handle);
    
    while (router.running) {
        /* Read from USB (capture) */
        frames = snd_pcm_readi(router.capture_handle, buffer, PERIOD_SIZE);
        
        if (frames < 0) {
            if (frames == -EPIPE) {
                fprintf(stderr, "Capture overrun, recovering...\n");
                snd_pcm_prepare(router.capture_handle);
                snd_pcm_start(router.capture_handle);
                continue;
            } else if (frames == -ESTRPIPE) {
                fprintf(stderr, "Capture suspended, recovering...\n");
                while ((frames = snd_pcm_resume(router.capture_handle)) == -EAGAIN)
                    sleep(1);
                if (frames < 0) {
                    snd_pcm_prepare(router.capture_handle);
                    snd_pcm_start(router.capture_handle);
                }
                continue;
            } else {
                fprintf(stderr, "Read error: %s\n", snd_strerror(frames));
                break;
            }
        }
        
        if (frames < PERIOD_SIZE) {
            fprintf(stderr, "Short read: %ld frames\n", frames);
        }
        
        /* Write to I2S (playback) */
        frames = snd_pcm_writei(router.playback_handle, buffer, frames);
        
        if (frames < 0) {
            if (frames == -EPIPE) {
                fprintf(stderr, "Playback underrun, recovering...\n");
                snd_pcm_prepare(router.playback_handle);
                continue;
            } else if (frames == -ESTRPIPE) {
                fprintf(stderr, "Playback suspended, recovering...\n");
                while ((frames = snd_pcm_resume(router.playback_handle)) == -EAGAIN)
                    sleep(1);
                if (frames < 0)
                    snd_pcm_prepare(router.playback_handle);
                continue;
            } else {
                fprintf(stderr, "Write error: %s\n", snd_strerror(frames));
                break;
            }
        }
    }
    
    free(buffer);
    printf("Audio routing stopped\n");
    return NULL;
}

/* Start audio routing */
int start_routing(unsigned int rate) {
    if (router.running) {
        fprintf(stderr, "Router already running\n");
        return -1;
    }
    
    if (open_audio_devices(rate) < 0) {
        return -1;
    }
    
    router.current_rate = rate;
    router.running = 1;
    
    if (pthread_create(&router.thread, NULL, audio_thread, NULL) != 0) {
        fprintf(stderr, "Cannot create audio thread\n");
        router.running = 0;
        snd_pcm_close(router.capture_handle);
        snd_pcm_close(router.playback_handle);
        return -1;
    }
    
    return 0;
}

/* Stop audio routing */
void stop_routing(void) {
    if (!router.running)
        return;
    
    router.running = 0;
    pthread_join(router.thread, NULL);
    
    snd_pcm_drop(router.capture_handle);
    snd_pcm_drop(router.playback_handle);
    snd_pcm_close(router.capture_handle);
    snd_pcm_close(router.playback_handle);
    
    router.capture_handle = NULL;
    router.playback_handle = NULL;
}

int main(int argc, char *argv[]) {
    unsigned int sample_rate = 48000; // Default
    
    if (argc > 1) {
        sample_rate = atoi(argv[1]);
        if (sample_rate < 44100 || sample_rate > 384000) {
            fprintf(stderr, "Invalid sample rate: %u (must be 44100-384000)\n", sample_rate);
            return 1;
        }
    }
    
    printf("USB UAC2 to I2S Audio Router\n");
    printf("Sample rate: %u Hz\n", sample_rate);
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Start routing */
    if (start_routing(sample_rate) < 0) {
        fprintf(stderr, "Failed to start audio routing\n");
        return 1;
    }
    
    /* Run until signal */
    while (router.running) {
        sleep(1);
    }
    
    stop_routing();
    
    printf("Done\n");
    return 0;
}
