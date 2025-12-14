#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int from_id;
    int duration_ms;
    bool ready;
    pthread_barrier_t *sync;
} CallInfo;

typedef struct {
    int id;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t incoming_cond;
    CallInfo incoming;
    bool active;
    bool busy;
    int conversations;
    struct SharedCondState *shared;
} Talker;

typedef struct SharedCondState {
    const Config *config;
    Logger *logger;
    Talker talkers[MAX_TALKERS];
    _Atomic int active_count;
    struct timespec start_ts;
    _Atomic bool stop;
} SharedCond;

static void msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static bool timed_out(const SharedCond *shared) {
    if (shared->config->duration_seconds <= 0) return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec = now.tv_sec - shared->start_ts.tv_sec;
    return sec >= shared->config->duration_seconds;
}

static bool should_leave(const Config *cfg, Talker *self) {
    if (cfg->stop_after_calls > 0 && self->conversations >= cfg->stop_after_calls) {
        return true;
    }
    double r = rand() / (double)RAND_MAX;
    return r < cfg->leave_probability;
}

static void leave_network(SharedCond *shared, Talker *self) {
    pthread_mutex_lock(&self->mutex);
    self->active = false;
    pthread_mutex_unlock(&self->mutex);
    int left = atomic_fetch_sub(&shared->active_count, 1) - 1;
    log_message(shared->logger, "Болтун %d отключился (осталось %d)", self->id, left);
}

static void finish(Talker *self, SharedCond *shared, int other_id, int duration_ms) {
    self->busy = false;
    self->conversations++;
    log_message(shared->logger, "Болтун %d завершил разговор с %d (%d мс)", self->id, other_id, duration_ms);
}

static void handle_incoming(SharedCond *shared, Talker *self) {
    pthread_mutex_lock(&self->mutex);
    while (self->incoming.ready) {
        CallInfo info = self->incoming;
        self->incoming.ready = false;
        self->incoming.sync = NULL;
        self->busy = true;
        pthread_mutex_unlock(&self->mutex);

        Talker *caller = &shared->talkers[info.from_id];
        log_message(shared->logger, "Болтун %d отвечает на звонок %d", self->id, caller->id);

        pthread_barrier_wait(info.sync);

        log_message(shared->logger, "Разговор %d ↔ %d (%d мс)", caller->id, self->id, info.duration_ms);
        msleep(info.duration_ms);

        finish(self, shared, caller->id, info.duration_ms);

        pthread_mutex_lock(&self->mutex);
    }
    pthread_mutex_unlock(&self->mutex);
}

static bool try_call(SharedCond *shared, Talker *self) {
    const Config *cfg = shared->config;
    int duration = random_range(cfg->min_call_ms, cfg->max_call_ms);
    int attempts = 0;

    while (attempts < cfg->talkers * 2 && !atomic_load(&shared->stop) && !timed_out(shared)) {
        int target = random_range(0, cfg->talkers - 1);
        if (target == self->id) { attempts++; continue; }
        Talker *callee = &shared->talkers[target];

        pthread_mutex_lock(&callee->mutex);
        bool available = callee->active && !callee->busy && !callee->incoming.ready;
        if (available) {
            callee->incoming.from_id = self->id;
            pthread_barrier_t *sync = malloc(sizeof(pthread_barrier_t));
            pthread_barrier_init(sync, NULL, 2);

            callee->incoming.duration_ms = duration;
            callee->incoming.ready = true;
            callee->incoming.sync = sync;
            callee->busy = true;
            pthread_cond_signal(&callee->incoming_cond);
            pthread_mutex_unlock(&callee->mutex);

            self->busy = true;
            log_message(shared->logger, "Болтун %d набирает %d", self->id, target);

            pthread_barrier_wait(sync);
            pthread_barrier_destroy(sync);
            free(sync);

            log_message(shared->logger, "Разговор %d ↔ %d (%d мс)", self->id, target, duration);
            msleep(duration);
            finish(self, shared, target, duration);
            return true;
        }
        pthread_mutex_unlock(&callee->mutex);
        log_message(shared->logger, "Линия %d занята для %d", target, self->id);
        attempts++;
    }
    return false;
}

static void *talker_thread(void *arg) {
    Talker *self = (Talker *)arg;
    SharedCond *shared = self->shared;
    const Config *cfg = shared->config;

    while (self->active && !atomic_load(&shared->stop) && !timed_out(shared)) {
        int pause_ms = random_range(cfg->min_idle_ms, cfg->max_idle_ms);
        msleep(pause_ms);

        handle_incoming(shared, self);
        if (!self->active || atomic_load(&shared->stop) || timed_out(shared)) break;

        if (random_range(0, 1) == 0) {
            // предпочтение ожиданию
            pthread_mutex_lock(&self->mutex);
            if (!self->incoming.ready) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 100000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec += 1; }
                pthread_cond_timedwait(&self->incoming_cond, &self->mutex, &ts);
            }
            pthread_mutex_unlock(&self->mutex);
            handle_incoming(shared, self);
        } else {
            try_call(shared, self);
        }

        if (should_leave(cfg, self)) {
            leave_network(shared, self);
            break;
        }
    }

    if (atomic_load(&shared->active_count) == 0) {
        log_message(shared->logger, "Последний болтун завершил работу");
    }
    return NULL;
}

int run_condition_mode(const Config *config, Logger *logger) {
    SharedCond shared = { .config = config, .logger = logger };
    shared.active_count = config->talkers;
    shared.stop = false;
    clock_gettime(CLOCK_MONOTONIC, &shared.start_ts);
    srand((unsigned)time(NULL) ^ 0x55aa);

    for (int i = 0; i < config->talkers; ++i) {
        Talker *t = &shared.talkers[i];
        t->id = i;
        t->shared = &shared;
        t->active = true;
        t->busy = false;
        t->incoming.ready = false;
        t->conversations = 0;
        pthread_mutex_init(&t->mutex, NULL);
        pthread_cond_init(&t->incoming_cond, NULL);
    }

    for (int i = 0; i < config->talkers; ++i) {
        pthread_create(&shared.talkers[i].thread, NULL, talker_thread, &shared.talkers[i]);
        log_message(logger, "Болтун %d подключился", i);
    }

    for (int i = 0; i < config->talkers; ++i) {
        pthread_join(shared.talkers[i].thread, NULL);
    }

    for (int i = 0; i < config->talkers; ++i) {
        pthread_mutex_destroy(&shared.talkers[i].mutex);
        pthread_cond_destroy(&shared.talkers[i].incoming_cond);
    }
    return 0;
}

