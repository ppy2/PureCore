#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <pthread.h>

#define UAC2_CARD "hw:1,0"
#define I2S_CARD "hw:0,0"
#define BUFFER_SIZE 512

static volatile sig_atomic_t keep_running = 1;
static snd_pcm_t *capture_handle = NULL;
static snd_pcm_t *playback_handle = NULL;
static pthread_mutex_t pcm_lock = PTHREAD_MUTEX_INITIALIZER;

void signal_handler(int sig)
{
	keep_running = 0;
}

void cleanup_pcm(void)
{
	pthread_mutex_lock(&pcm_lock);
	if (capture_handle) {
		snd_pcm_close(capture_handle);
		capture_handle = NULL;
	}
	if (playback_handle) {
		snd_pcm_close(playback_handle);
		playback_handle = NULL;
	}
	pthread_mutex_unlock(&pcm_lock);
}

int configure_pcm(snd_pcm_t *pcm, snd_pcm_stream_t stream, unsigned int rate,
		  snd_pcm_format_t format, unsigned int channels)
{
	snd_pcm_hw_params_t *hw_params;
	int err;

	if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
		fprintf(stderr, "cannot allocate hw params (%s)\n", snd_strerror(err));
		return err;
	}

	if ((err = snd_pcm_hw_params_any(pcm, hw_params)) < 0) {
		fprintf(stderr, "cannot initialize hw params (%s)\n", snd_strerror(err));
		snd_pcm_hw_params_free(hw_params);
		return err;
	}

	if ((err = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
		fprintf(stderr, "cannot set access type (%s)\n", snd_strerror(err));
		snd_pcm_hw_params_free(hw_params);
		return err;
	}

	if ((err = snd_pcm_hw_params_set_format(pcm, hw_params, format)) < 0) {
		fprintf(stderr, "cannot set format (%s)\n", snd_strerror(err));
		snd_pcm_hw_params_free(hw_params);
		return err;
	}

	if ((err = snd_pcm_hw_params_set_rate(pcm, hw_params, rate, 0)) < 0) {
		fprintf(stderr, "cannot set rate (%s)\n", snd_strerror(err));
		snd_pcm_hw_params_free(hw_params);
		return err;
	}

	if ((err = snd_pcm_hw_params_set_channels(pcm, hw_params, channels)) < 0) {
		fprintf(stderr, "cannot set channels (%s)\n", snd_strerror(err));
		snd_pcm_hw_params_free(hw_params);
		return err;
	}

	if ((err = snd_pcm_hw_params(pcm, hw_params)) < 0) {
		fprintf(stderr, "cannot set hw params (%s)\n", snd_strerror(err));
		snd_pcm_hw_params_free(hw_params);
		return err;
	}

	snd_pcm_hw_params_free(hw_params);
	return 0;
}

int detect_uac2_params(unsigned int *rate, snd_pcm_format_t *format, unsigned int *channels)
{
	FILE *fp;
	char line[256];
	int found_rate = 0, found_format = 0, found_channels = 0;

	fp = fopen("/proc/asound/card1/pcm0c/sub0/hw_params", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "rate: %u", rate) == 1) {
			found_rate = 1;
		} else if (strstr(line, "format: S16_LE")) {
			*format = SND_PCM_FORMAT_S16_LE;
			found_format = 1;
		} else if (strstr(line, "format: S24_3LE")) {
			*format = SND_PCM_FORMAT_S24_3LE;
			found_format = 1;
		} else if (strstr(line, "format: S24_LE")) {
			*format = SND_PCM_FORMAT_S24_LE;
			found_format = 1;
		} else if (strstr(line, "format: S32_LE")) {
			*format = SND_PCM_FORMAT_S32_LE;
			found_format = 1;
		} else if (sscanf(line, "channels: %u", channels) == 1) {
			found_channels = 1;
		}
	}

	fclose(fp);
	return (found_rate && found_format && found_channels) ? 0 : -1;
}

int setup_routing(unsigned int rate, snd_pcm_format_t format, unsigned int channels)
{
	int err;

	pthread_mutex_lock(&pcm_lock);

	cleanup_pcm();

	printf("Setting up routing: %u Hz, format %d, %u channels\n", rate, format, channels);

	if ((err = snd_pcm_open(&capture_handle, UAC2_CARD, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
		fprintf(stderr, "cannot open UAC2 capture (%s)\n", snd_strerror(err));
		pthread_mutex_unlock(&pcm_lock);
		return err;
	}

	if ((err = configure_pcm(capture_handle, SND_PCM_STREAM_CAPTURE, rate, format, channels)) < 0) {
		snd_pcm_close(capture_handle);
		capture_handle = NULL;
		pthread_mutex_unlock(&pcm_lock);
		return err;
	}

	if ((err = snd_pcm_open(&playback_handle, I2S_CARD, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		fprintf(stderr, "cannot open I2S playback (%s)\n", snd_strerror(err));
		snd_pcm_close(capture_handle);
		capture_handle = NULL;
		pthread_mutex_unlock(&pcm_lock);
		return err;
	}

	if ((err = configure_pcm(playback_handle, SND_PCM_STREAM_PLAYBACK, rate, format, channels)) < 0) {
		snd_pcm_close(capture_handle);
		snd_pcm_close(playback_handle);
		capture_handle = NULL;
		playback_handle = NULL;
		pthread_mutex_unlock(&pcm_lock);
		return err;
	}

	pthread_mutex_unlock(&pcm_lock);
	return 0;
}

void *routing_thread(void *arg)
{
	unsigned char buffer[BUFFER_SIZE * 4 * 8];
	snd_pcm_sframes_t frames;

	while (keep_running) {
		pthread_mutex_lock(&pcm_lock);
		
		if (!capture_handle || !playback_handle) {
			pthread_mutex_unlock(&pcm_lock);
			usleep(100000);
			continue;
		}

		frames = snd_pcm_readi(capture_handle, buffer, BUFFER_SIZE);
		
		if (frames < 0) {
			frames = snd_pcm_recover(capture_handle, frames, 0);
			if (frames < 0) {
				fprintf(stderr, "capture error: %s\n", snd_strerror(frames));
				pthread_mutex_unlock(&pcm_lock);
				usleep(10000);
				continue;
			}
			pthread_mutex_unlock(&pcm_lock);
			continue;
		}

		if (frames > 0) {
			snd_pcm_sframes_t wframes = snd_pcm_writei(playback_handle, buffer, frames);
			if (wframes < 0) {
				wframes = snd_pcm_recover(playback_handle, wframes, 0);
				if (wframes < 0) {
					fprintf(stderr, "playback error: %s\n", snd_strerror(wframes));
				}
			}
		}

		pthread_mutex_unlock(&pcm_lock);
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	unsigned int rate, channels;
	snd_pcm_format_t format;
	pthread_t thread;
	int inotify_fd, watch_fd;
	char event_buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	inotify_fd = inotify_init1(IN_NONBLOCK);
	if (inotify_fd < 0) {
		perror("inotify_init1");
		return 1;
	}

	watch_fd = inotify_add_watch(inotify_fd, "/proc/asound/card1/pcm0c/sub0", IN_MODIFY | IN_CREATE);
	if (watch_fd < 0) {
		fprintf(stderr, "Waiting for UAC2 card...\n");
	}

	if (pthread_create(&thread, NULL, routing_thread, NULL) != 0) {
		perror("pthread_create");
		close(inotify_fd);
		return 1;
	}

	printf("UAC2->I2S router started\n");

	while (keep_running) {
		if (detect_uac2_params(&rate, &format, &channels) == 0) {
			setup_routing(rate, format, channels);
		}

		sleep(1);

		if (watch_fd >= 0) {
			ssize_t len = read(inotify_fd, event_buf, sizeof(event_buf));
			if (len > 0) {
				usleep(50000);
				if (detect_uac2_params(&rate, &format, &channels) == 0) {
					setup_routing(rate, format, channels);
				}
			}
		}
	}

	pthread_join(thread, NULL);
	cleanup_pcm();
	close(inotify_fd);
	printf("UAC2->I2S router stopped\n");

	return 0;
}
