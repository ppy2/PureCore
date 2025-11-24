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
#include <poll.h>

#define UAC2_CARD "hw:1,0"
#define I2S_CARD "hw:0,0"
#define PERIOD_FRAMES 512  /* Баланс между CPU и равномерностью нагрузки */

/* Новый sysfs интерфейс от модифицированного драйвера u_audio.c */
#define SYSFS_UAC2_PATH "/sys/class/u_audio"
#define SYSFS_RATE_FILE "rate"
#define SYSFS_FORMAT_FILE "format"
#define SYSFS_CHANNELS_FILE "channels"

/* Фиксированный формат для I2S (как в оригинальном XingCore) */
#define I2S_FORMAT_PCM SND_PCM_FORMAT_S32_LE  /* PCM: 32-бит */
#define I2S_FORMAT_DSD SND_PCM_FORMAT_DSD_U32_LE  /* DSD: 32-бит DSD */
#define I2S_CHANNELS 2                     /* Всегда стерео */

/* DSD sample rates (native DSD64/128/256/512) */
#define DSD64_RATE   2822400
#define DSD128_RATE  5644800
#define DSD256_RATE  11289600
#define DSD512_RATE  22579200

/* Размер буфера для netlink uevent */
#define UEVENT_BUFFER_SIZE 4096

static volatile int running = 1;
static snd_pcm_t *pcm_capture = NULL;
static snd_pcm_t *pcm_playback = NULL;
static char uac_card_path[256] = "";
static char uac_card_name[64] = "";
static int consecutive_errors = 0;
#define MAX_CONSECUTIVE_ERRORS 50  /* После 50 ошибок подряд (~0.5 сек) - переоткрыть PCM */

/* Volume synchronization */
static snd_mixer_t *uac2_mixer = NULL;
static snd_mixer_t *i2s_mixer = NULL;
static snd_mixer_elem_t *uac2_volume_elem = NULL;
static snd_mixer_elem_t *i2s_volume_elem = NULL;
static long last_uac2_volume = -1;
static int last_uac2_mute = -1;

static void sighandler(int sig) {
    running = 0;
}

/* Определить, является ли частота DSD */
static int is_dsd_rate(unsigned int rate) {
    return (rate == DSD64_RATE || rate == DSD128_RATE ||
            rate == DSD256_RATE || rate == DSD512_RATE);
}

/* Получить название DSD формата по частоте */
static const char* get_dsd_name(unsigned int rate) {
    switch (rate) {
        case DSD64_RATE:  return "DSD64";
        case DSD128_RATE: return "DSD128";
        case DSD256_RATE: return "DSD256";
        case DSD512_RATE: return "DSD512";
        default: return "Unknown";
    }
}

/* Инициализация volume sync: открыть mixers для UAC2 и I2S */
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

/* Синхронизировать volume: читать UAC2, применять к I2S */
static void sync_volume(void) {
    long uac2_vol, i2s_vol;
    int uac2_mute, i2s_mute;

    if (!uac2_volume_elem || !i2s_volume_elem)
        return;

    /* Обновить mixer state перед чтением (критично!) */
    snd_mixer_handle_events(uac2_mixer);

    /* Читать UAC2 volume и mute */
    snd_mixer_selem_get_capture_volume(uac2_volume_elem, SND_MIXER_SCHN_MONO, &uac2_vol);
    snd_mixer_selem_get_capture_switch(uac2_volume_elem, SND_MIXER_SCHN_MONO, &uac2_mute);

    /* Только если изменилось (проверяем И volume И mute) */
    if (uac2_vol != last_uac2_volume || uac2_mute != last_uac2_mute) {
        /* Применить к I2S */
        snd_mixer_selem_set_playback_volume_all(i2s_volume_elem, uac2_vol);
        snd_mixer_selem_set_playback_switch_all(i2s_volume_elem, uac2_mute);

        last_uac2_volume = uac2_vol;
        last_uac2_mute = uac2_mute;
        printf("[VOLUME] Synced: %ld%% %s\n", uac2_vol, uac2_mute ? "ON" : "MUTE");
    }
}

/* Закрыть mixers */
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

/* Найти UAC карту в /sys/class/u_audio/ */
static int find_uac_card(void) {
    char path[256];
    struct stat st;

    /* Попробуем uac_card0, uac_card1, etc */
    for (int i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "%s/uac_card%d", SYSFS_UAC2_PATH, i);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(uac_card_path, path, sizeof(uac_card_path) - 1);
            snprintf(uac_card_name, sizeof(uac_card_name), "uac_card%d", i);
            printf("Found UAC card: %s (card %d)\n", uac_card_path, i);
            return i;  // Возвращаем номер карты
        }
    }

    fprintf(stderr, "ERROR: No UAC card found in %s\n", SYSFS_UAC2_PATH);
    return -1;
}

/* Читать значение из sysfs файла */
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

/* Создать netlink socket для uevent */
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

/* Настроить PCM устройство */
static int setup_pcm(snd_pcm_t **pcm, const char *device, snd_pcm_stream_t stream,
                     unsigned int rate, snd_pcm_format_t format, unsigned int channels)
{
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    int err;
    /* Адаптивный размер периода: больше для высоких частот (DSD) */
    snd_pcm_uframes_t period_size = PERIOD_FRAMES;
    if (rate >= DSD64_RATE) {
        /* DSD rates: увеличиваем период чтобы избежать слишком коротких интервалов
         * DSD64:  2.8MHz → 2048 frames = 0.7ms
         * DSD128: 5.6MHz → 4096 frames = 0.7ms
         * DSD256: 11.2MHz → 8192 frames = 0.7ms
         * DSD512: 22.5MHz → 16384 frames = 0.7ms */
        period_size = (rate / DSD64_RATE) * 2048;
    }
    snd_pcm_uframes_t buffer_size = period_size * 4;

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

/* Закрыть PCM устройства */
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

/* Настроить аудио с заданной частотой */
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

    /* UAC2 capture - всегда PCM S32_LE (USB передает DSD как "raw" 32-bit data) */
    char uac_device[32];
    sprintf(uac_device, "hw:%d,0", card);
    if (setup_pcm(&pcm_capture, uac_device, SND_PCM_STREAM_CAPTURE,
                  rate, I2S_FORMAT_PCM, I2S_CHANNELS) < 0) {
        return -1;
    }

    /* I2S playback - PCM или DSD формат в зависимости от частоты */
    if (setup_pcm(&pcm_playback, I2S_CARD, SND_PCM_STREAM_PLAYBACK,
                  rate, i2s_format, I2S_CHANNELS) < 0) {
        close_pcms();
        return -1;
    }

    if (is_dsd) {
        printf("[CONFIG] DSD stream configured: USB→I2S routing active\n");
    }

    /* Получить период размеры для обоих устройств */
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);

    snd_pcm_hw_params_current(pcm_capture, hw);
    snd_pcm_hw_params_get_period_size(hw, &capture_period_size, 0);

    snd_pcm_hw_params_current(pcm_playback, hw);
    snd_pcm_hw_params_get_period_size(hw, &playback_period_size, 0);

    /* Используем меньший период для чтения */
    *period_size_out = capture_period_size;

    /* Выделить буфер размером с больший период (для накопления данных) */
    snd_pcm_uframes_t max_period = (capture_period_size > playback_period_size) ?
                                    capture_period_size : playback_period_size;
    /* Используем PCM формат для расчета буфера (DSD и PCM оба 32-бит) */
    size_t frame_bytes = snd_pcm_format_physical_width(I2S_FORMAT_PCM) / 8 * I2S_CHANNELS;
    *buffer_size = max_period * frame_bytes;
    *buffer = realloc(*buffer, *buffer_size);

    if (!*buffer) {
        fprintf(stderr, "Cannot allocate buffer\n");
        close_pcms();
        return -1;
    }

    /* Подготовить оба устройства */
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

    /* Запустить capture (playback запустится автоматически при первой записи) */
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

    printf("═══════════════════════════════════════════════════════════\n");
    printf("  UAC2 -> I2S Router (uevent-based, XingCore compatible)\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    /* Найти UAC карту */
    uac_card = find_uac_card();
    if (uac_card < 0) {
        fprintf(stderr, "ERROR: UAC2 device not found. Is gadget loaded?\n");
        return 1;
    }

    /* Прочитать статичные параметры (format и channels) */
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

    /* Создать netlink socket для uevent */
    uevent_sock = create_uevent_socket();
    if (uevent_sock < 0) {
        fprintf(stderr, "ERROR: Cannot create uevent socket\n");
        return 1;
    }

    printf("Listening for kobject uevent from u_audio driver...\n");
    printf("Waiting for rate changes...\n\n");

    /* Прочитать начальную частоту и настроить аудио */
    int rate = read_sysfs_int(SYSFS_RATE_FILE);
    if (rate > 0) {
        printf("Initial rate: %d Hz\n", rate);
        if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
            current_rate = rate;
        }
    }

    /* Volume sync отключен - UAC2 не имеет mixer controls */
    printf("[VOLUME] Volume sync disabled - UAC2 has no mixer controls\n");

    /* Основной цикл */
    while (running) {
        /* Неблокирующая проверка uevent (MSG_DONTWAIT очень быстрый если нет события) */
        ssize_t len = recv(uevent_sock, uevent_buf, sizeof(uevent_buf) - 1, MSG_DONTWAIT);
        if (len > 0) {
            uevent_buf[len] = '\0';

            /* Проверить, что это событие от нашей UAC карты */
            if (strstr(uevent_buf, "u_audio") && strstr(uevent_buf, uac_card_name)) {
                /* Прочитать новую частоту */
                rate = read_sysfs_int(SYSFS_RATE_FILE);

                if (rate > 0 && rate != current_rate) {
                    printf("\n[CHANGE] Rate changed: %u Hz -> %u Hz\n", current_rate, rate);

                    if (configure_audio(rate, uac_card, &buffer, &buffer_size, &period_size) == 0) {
                        current_rate = rate;
                    }
                }
            }
        }

        /* Маршрутизация аудио */
        if (!pcm_capture || !pcm_playback) {
            usleep(100000);  /* 100ms */
            continue;
        }

        /* Ждать когда данные доступны (снижает CPU нагрузку) */
        int err = snd_pcm_wait(pcm_capture, 100);  /* Timeout 100ms */
        if (err <= 0) {
            if (err == 0) {
                /* Timeout - это нормально, продолжаем */
                consecutive_errors = 0;  /* Сброс счетчика */
                continue;
            }
            /* Ошибка - увеличиваем счетчик */
            consecutive_errors++;

            /* Слишком много ошибок подряд - переоткрыть PCM */
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                fprintf(stderr, "[ERROR] Too many consecutive errors (%d), reopening PCM devices...\n",
                        consecutive_errors);
                close_pcms();
                consecutive_errors = 0;
                usleep(500000);  /* 500ms перед переоткрытием */
                continue;
            }

            /* Обработка ошибки */
            if (err == -EPIPE) {
                /* XRUN - попробуем восстановить */
                fprintf(stderr, "[WARN] snd_pcm_wait: XRUN, recovering... (error #%d)\n", consecutive_errors);
                snd_pcm_prepare(pcm_capture);
                snd_pcm_prepare(pcm_playback);
            } else {
                /* Другая ошибка - логируем и делаем задержку */
                fprintf(stderr, "[ERROR] snd_pcm_wait failed: %s (%d) (error #%d)\n",
                        snd_strerror(err), err, consecutive_errors);
                usleep(10000);  /* 10ms - предотвращает tight loop */
            }
            continue;
        }

        /* Успешно получили данные - сброс счетчика ошибок */
        consecutive_errors = 0;

        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_capture, buffer, period_size);

        if (frames < 0) {
            consecutive_errors++;

            /* Защита от бесконечных ошибок чтения */
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
                snd_pcm_prepare(pcm_playback);
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
            snd_pcm_sframes_t written = snd_pcm_writei(pcm_playback, buffer, frames);
            if (written < 0) {
                if (written == -EPIPE) {
                    fprintf(stderr, "[XRUN] Playback underrun\n");
                    snd_pcm_prepare(pcm_playback);
                } else if (written == -ESTRPIPE) {
                    while ((written = snd_pcm_resume(pcm_playback)) == -EAGAIN)
                        usleep(10000);
                    if (written < 0)
                        snd_pcm_prepare(pcm_playback);
                } else if (written == -ENODEV || written == -EBADF) {
                    printf("[ERROR] Playback error, reinitializing...\n");
                    close_pcms();
                    usleep(500000);
                }
                continue;
            }
        }

        /* Volume sync отключен */
    }

    /* Cleanup */
    if (uevent_sock >= 0)
        close(uevent_sock);

    if (buffer)
        free(buffer);

    close_pcms();
    /* close_mixers() отключен - не используется */

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  UAC2 -> I2S Router stopped\n");
    printf("═══════════════════════════════════════════════════════════\n");

    return 0;
}
