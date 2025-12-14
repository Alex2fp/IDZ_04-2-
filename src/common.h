#ifndef COMMON_H
#define COMMON_H

#define _XOPEN_SOURCE 700

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <time.h>

#define MODE_SEMAPHORE "semaphore"
#define MODE_CONDITION "condition"

#define MAX_TALKERS 64
#define MAX_PATH_LEN 256

typedef struct {
    int talkers;
    int min_idle_ms;
    int max_idle_ms;
    int min_call_ms;
    int max_call_ms;
    int stop_after_calls; // <=0 to ignore
    double leave_probability; // 0..1
    int duration_seconds; // <=0 to ignore
    char output_path[MAX_PATH_LEN];
    char config_path[MAX_PATH_LEN];
    char mode[16];
} Config;

typedef struct {
    FILE *file;
    pthread_mutex_t lock;
    struct timespec start_ts;
} Logger;

int random_range(int min, int max);
bool stop_requested(void);
bool parse_args(int argc, char **argv, Config *config);
bool load_config_file(const char *path, Config *config);
void init_logger(Logger *logger, const char *path);
void close_logger(Logger *logger);
void log_message(Logger *logger, const char *fmt, ...);
long elapsed_ms_since(Logger *logger);

int run_semaphore_mode(const Config *config, Logger *logger);
int run_condition_mode(const Config *config, Logger *logger);

#endif // COMMON_H
