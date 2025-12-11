#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <time.h>

#define BUFFER_AVG_SAMPLES 50   /* 5 seconds at 10Hz */

static double buffer_history[BUFFER_AVG_SAMPLES];
static int buffer_history_index = 0;
static int buffer_history_filled = 0;
static unsigned int current_rate = 768000;
static int verbose_logging = 0;  /* Default: no logging */

static int send_feedback(unsigned int feedback_value) {
    FILE *fp = fopen("/sys/devices/virtual/u_audio/uac_card1/feedback", "w");
    if (!fp) {
        if (verbose_logging) printf("[FEEDBACK] Cannot open feedback sysfs\n");
        return -1;
    }
    
    fprintf(fp, "%u\n", feedback_value);
    fclose(fp);
    
    return 0;
}

static void read_rate_from_sysfs() {
    FILE *fp = fopen("/sys/devices/virtual/u_audio/uac_card1/rate", "r");
    if (fp) {
        unsigned int rate;
        if (fscanf(fp, "%u", &rate) == 1) {
            if (rate != current_rate) {
                if (verbose_logging) printf("[RATE] Changed from %u to %u Hz\n", current_rate, rate);
                current_rate = rate;
            }
        }
        fclose(fp);
    }
}

static int read_buffer_status(long *avail, long *buffer_size, long *delay) {
    FILE *fp = fopen("/proc/asound/card0/pcm0p/sub0/status", "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    *avail = -1;
    *buffer_size = 16384; /* Default from hw_params */
    *delay = -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "avail")) {
            sscanf(line, "avail       : %ld", avail);
        } else if (strstr(line, "delay")) {
            sscanf(line, "delay       : %ld", delay);
        }
    }
    fclose(fp);

    /* Filter out invalid values */
    if (*avail < 0 || *avail > *buffer_size) {
        return -1;
    }
    
    if (*delay < 0 || *delay > (*buffer_size * 2)) {
        return -1;
    }

    return 0;
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -v, --verbose    Enable verbose logging (default: disabled)\n");
    printf("  -h, --help       Show this help message\n");
    printf("\n");
    printf("By default, runs silently without logging output.\n");
    printf("Use -v to enable detailed monitoring information.\n");
}

int main(int argc, char *argv[]) {
    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose_logging = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (verbose_logging) {
        printf("═══════════════════════════════════════════════════════\n");
        printf("  Buffer Monitoring Daemon v4.0\n");
        printf("  Filters: ignore empty/invalid values\n");
        printf("  Verbose logging: ENABLED\n");
        printf("═══════════════════════════════════════════════════════\n");
        printf("Starting buffer monitoring with %d sample averaging...\n", BUFFER_AVG_SAMPLES);
    }

    /* Main buffer monitoring loop */
    while (1) {
        /* Read current rate */
        read_rate_from_sysfs();
        
        /* Read buffer status with filtering */
        long avail, buffer_size, delay;
        if (read_buffer_status(&avail, &buffer_size, &delay) < 0) {
            if (verbose_logging) printf("[FILTER] Invalid buffer status - skipping\n");
            usleep(100000); /* 100ms */
            continue;
        }

        /* Additional filtering: skip if delay is 0 or avail is buffer_size (empty) */
        if (delay == 0 || avail == buffer_size) {
            if (verbose_logging) printf("[FILTER] Empty buffer (delay=%ld, avail=%ld) - skipping\n", delay, avail);
            usleep(100000); /* 100ms */
            continue;
        }

        /* Calculate current buffer fill ratio */
        double current_fill = (double)(buffer_size - avail) / buffer_size;
        
        /* Add to history buffer */
        buffer_history[buffer_history_index] = current_fill;
        buffer_history_index = (buffer_history_index + 1) % BUFFER_AVG_SAMPLES;
        
        if (!buffer_history_filled) {
            if (buffer_history_index == 0) {
                buffer_history_filled = 1;
            }
        }
        
        /* Calculate average fill ratio */
        double fill_ratio = 0.0;
        int samples = buffer_history_filled ? BUFFER_AVG_SAMPLES : buffer_history_index;
        for (int i = 0; i < samples; i++) {
            fill_ratio += buffer_history[i];
        }
        fill_ratio /= samples;
        
        /* Calculate delay in milliseconds */
        double delay_ms = (double)delay * 1000.0 / (current_rate * 2 * 4);
        
        if (verbose_logging) {
            printf("[MONITOR] delay=%ld, avail=%ld, size=%ld, current=%.3f, avg=%.3f, delay_ms=%.2f\n", 
                   delay, avail, buffer_size, current_fill, fill_ratio, delay_ms);
        }
        
/* Feedback control with hysteresis and gradual adjustment */
        static unsigned int current_feedback = 1000000;
        unsigned int target_feedback = current_feedback;
        
        /* Hysteresis zones with overlap to prevent oscillation */
        if (fill_ratio > 0.65) {
            /* Buffer very high - need slowdown */
            target_feedback = 999000;
            if (verbose_logging) printf("[ACTION] Buffer very high (%.3f) - target %u\n", fill_ratio, target_feedback);
        } else if (fill_ratio > 0.45) {
            /* Buffer high - slight slowdown */
            target_feedback = 999500;
            if (verbose_logging) printf("[ACTION] Buffer high (%.3f) - target %u\n", fill_ratio, target_feedback);
        } else if (fill_ratio > 0.35) {
            /* Buffer moderate - maintain nominal */
            target_feedback = 1000000;
            if (verbose_logging) printf("[ACTION] Buffer moderate (%.3f) - target %u\n", fill_ratio, target_feedback);
        } else if (fill_ratio < 0.20) {
            /* Buffer very low - speedup */
            target_feedback = 1002000;
            if (verbose_logging) printf("[ACTION] Buffer very low (%.3f) - target %u\n", fill_ratio, target_feedback);
        } else if (fill_ratio < 0.30) {
            /* Buffer low - slight speedup */
            target_feedback = 1001000;
            if (verbose_logging) printf("[ACTION] Buffer low (%.3f) - target %u\n", fill_ratio, target_feedback);
        } else {
            /* Buffer in good range - maintain nominal */
            target_feedback = 1000000;
            if (verbose_logging) printf("[ACTION] Buffer good (%.3f) - target %u\n", fill_ratio, target_feedback);
        }
        
        /* Gradual adjustment - max 0.02% change per cycle for ultra-smooth control */
        if (target_feedback != current_feedback) {
            int max_change = 200; /* 0.02% of 1000000 - reduced from 500 for even slower changes */
            int feedback_diff = target_feedback - current_feedback;
            
            if (feedback_diff > max_change) {
                current_feedback += max_change;
                if (verbose_logging) printf("[GRADUAL] Increase to %u (+%d)\n", current_feedback, max_change);
            } else if (feedback_diff < -max_change) {
                current_feedback -= max_change;
                if (verbose_logging) printf("[GRADUAL] Decrease to %u (-%d)\n", current_feedback, max_change);
            } else {
                current_feedback = target_feedback;
                if (verbose_logging) printf("[GRADUAL] Target reached %u\n", current_feedback);
            }
        } else {
            if (verbose_logging) printf("[GRADUAL] Maintain %u (no change)\n", current_feedback);
        }
        
        send_feedback(current_feedback);
        
        usleep(100000); /* 100ms = 10Hz sampling */
    }

    return 0;
}