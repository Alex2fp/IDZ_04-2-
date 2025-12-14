#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

typedef struct {
    const char *key;
    enum { CFG_INT, CFG_DOUBLE, CFG_STRING } type;
    void *target;
    size_t max_len;
} ConfigEntry;

static _Atomic bool stop_flag = false;

static void on_signal(int signum) {
    (void)signum;
    stop_flag = true;
}

bool stop_requested(void) {
    return atomic_load(&stop_flag);
}

int random_range(int min, int max) {
    if (max <= min) {
        return min;
    }
    int span = max - min + 1;
    return min + rand() % span;
}

static void trim(char *s) {
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start) {
        memmove(s, s + start, len - start + 1);
    }
}

static bool parse_line(const char *line, char *key, char *value) {
    const char *eq = strchr(line, '=');
    if (!eq) return false;
    size_t klen = (size_t)(eq - line);
    strncpy(key, line, klen);
    key[klen] = '\0';
    strcpy(value, eq + 1);
    trim(key);
    trim(value);
    return strlen(key) > 0;
}

bool load_config_file(const char *path, Config *config) {
    if (!path || !*path) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    ConfigEntry table[] = {
        {"talkers", CFG_INT, &config->talkers, 0},
        {"min_idle_ms", CFG_INT, &config->min_idle_ms, 0},
        {"max_idle_ms", CFG_INT, &config->max_idle_ms, 0},
        {"min_call_ms", CFG_INT, &config->min_call_ms, 0},
        {"max_call_ms", CFG_INT, &config->max_call_ms, 0},
        {"stop_after_calls", CFG_INT, &config->stop_after_calls, 0},
        {"leave_probability", CFG_DOUBLE, &config->leave_probability, 0},
        {"duration_seconds", CFG_INT, &config->duration_seconds, 0},
        {"output", CFG_STRING, config->output_path, MAX_PATH_LEN},
        {"mode", CFG_STRING, config->mode, sizeof(config->mode)},
    };

    char line[256];
    char key[128];
    char value[128];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (!parse_line(line, key, value)) continue;
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
            if (strcmp(table[i].key, key) == 0) {
                if (table[i].type == CFG_INT) {
                    *(int *)table[i].target = atoi(value);
                } else if (table[i].type == CFG_DOUBLE) {
                    *(double *)table[i].target = atof(value);
                } else if (table[i].type == CFG_STRING) {
                    size_t limit = table[i].max_len ? table[i].max_len : MAX_PATH_LEN;
                    strncpy((char *)table[i].target, value, limit - 1);
                    ((char *)table[i].target)[limit - 1] = '\0';
                }
            }
        }
    }

    fclose(f);
    return true;
}

bool parse_args(int argc, char **argv, Config *config) {
    // defaults
    config->talkers = 4;
    config->min_idle_ms = 200;
    config->max_idle_ms = 800;
    config->min_call_ms = 300;
    config->max_call_ms = 1200;
    config->stop_after_calls = 0;
    config->leave_probability = 0.2;
    config->duration_seconds = 10;
    strcpy(config->output_path, "outputs/run.log");
    strcpy(config->mode, MODE_SEMAPHORE);
    config->config_path[0] = '\0';

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            strncpy(config->config_path, argv[++i], MAX_PATH_LEN - 1);
            config->config_path[MAX_PATH_LEN - 1] = '\0';
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--talkers") == 0) && i + 1 < argc) {
            config->talkers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--min-idle") == 0 && i + 1 < argc) {
            config->min_idle_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-idle") == 0 && i + 1 < argc) {
            config->max_idle_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--min-call") == 0 && i + 1 < argc) {
            config->min_call_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-call") == 0 && i + 1 < argc) {
            config->max_call_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stop-after-calls") == 0 && i + 1 < argc) {
            config->stop_after_calls = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--leave-probability") == 0 && i + 1 < argc) {
            config->leave_probability = atof(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            config->duration_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            strncpy(config->output_path, argv[++i], MAX_PATH_LEN - 1);
            config->output_path[MAX_PATH_LEN - 1] = '\0';
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            strncpy(config->mode, argv[++i], sizeof(config->mode) - 1);
            config->mode[sizeof(config->mode) - 1] = '\0';
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --config <file>          конфигурационный файл (key=value)\n");
            printf("  -n, --talkers <N>        число болтунов (1-%d)\n", MAX_TALKERS);
            printf("  --min-idle <ms>          минимальная пауза ожидания\n");
            printf("  --max-idle <ms>          максимальная пауза ожидания\n");
            printf("  --min-call <ms>          минимальная длительность звонка\n");
            printf("  --max-call <ms>          максимальная длительность звонка\n");
            printf("  --stop-after-calls <n>   отключение после n разговоров (0 — нет лимита)\n");
            printf("  --leave-probability <p>  вероятность ухода после разговора (0..1)\n");
            printf("  --duration <sec>         ограничение по времени работы\n");
            printf("  --output <path>          файл лога (пусто — только консоль)\n");
            printf("  --mode <semaphore|condition> выбор реализации синхронизации\n");
            return false;
        }
    }

    if (config->config_path[0]) {
        load_config_file(config->config_path, config);
    }

    if (config->talkers < 1 || config->talkers > MAX_TALKERS) {
        fprintf(stderr, "Некорректное число болтунов\n");
        return false;
    }
    if (config->min_idle_ms <= 0 || config->max_idle_ms < config->min_idle_ms) return false;
    if (config->min_call_ms <= 0 || config->max_call_ms < config->min_call_ms) return false;
    if (config->leave_probability < 0.0 || config->leave_probability > 1.0) return false;
    if (strcmp(config->mode, MODE_SEMAPHORE) != 0 && strcmp(config->mode, MODE_CONDITION) != 0) return false;

    return true;
}

void init_logger(Logger *logger, const char *path) {
    pthread_mutex_init(&logger->lock, NULL);
    clock_gettime(CLOCK_MONOTONIC, &logger->start_ts);
    logger->file = NULL;
    if (path && *path) {
        logger->file = fopen(path, "w");
        if (!logger->file) {
            perror("fopen log");
        }
    }
    signal(SIGINT, on_signal);
}

void close_logger(Logger *logger) {
    if (logger->file) fclose(logger->file);
    pthread_mutex_destroy(&logger->lock);
}

long elapsed_ms_since(Logger *logger) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec = now.tv_sec - logger->start_ts.tv_sec;
    long nsec = now.tv_nsec - logger->start_ts.tv_nsec;
    return sec * 1000 + nsec / 1000000;
}

void log_message(Logger *logger, const char *fmt, ...) {
    pthread_mutex_lock(&logger->lock);
    long ms = elapsed_ms_since(logger);
    printf("[%6ld ms] ", ms);
    if (logger->file) fprintf(logger->file, "[%6ld ms] ", ms);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    if (logger->file) {
        va_list args2;
        va_start(args2, fmt);
        vfprintf(logger->file, fmt, args2);
        va_end(args2);
    }
    va_end(args);

    printf("\n");
    if (logger->file) {
        fprintf(logger->file, "\n");
        fflush(logger->file);
    }
    fflush(stdout);
    pthread_mutex_unlock(&logger->lock);
}

