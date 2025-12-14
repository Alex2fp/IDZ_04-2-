#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int from_id;
    int duration_ms;
    bool has_request;
} CallRequest;

typedef struct {
    int id;
    pthread_t thread;
    pthread_mutex_t mutex;
    sem_t incoming_sem;
    sem_t answer_sem;
    CallRequest incoming;
    bool active;
    bool busy;
    int conversations;
    struct SharedState *shared;
} Talker;

typedef struct SharedState {
    const Config *config;
    Logger *logger;
    Talker talkers[MAX_TALKERS];
    _Atomic int active_count;
    struct timespec start_ts;
} Shared;

static void msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static bool timed_out(const Shared *shared) {
    if (shared->config->duration_seconds <= 0) return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec = now.tv_sec - shared->start_ts.tv_sec;
    return sec >= shared->config->duration_seconds;
}

static void release_self(Talker *self) {
    pthread_mutex_lock(&self->mutex);
    self->busy = false;
    pthread_mutex_unlock(&self->mutex);
}

static void finish_conversation(Shared *shared, Talker *self, int other_id, int duration_ms) {
    log_message(shared->logger, "Болтун %d завершил разговор с %d (%d мс)", self->id, other_id, duration_ms);
    release_self(self);
    self->conversations++;
}

static bool should_leave(const Config *cfg, Talker *self) {
    if (cfg->stop_after_calls > 0 && self->conversations >= cfg->stop_after_calls) {
        return true;
    }
    double r = rand() / (double)RAND_MAX;
    return r < cfg->leave_probability;
}

static void leave_network(Shared *shared, Talker *self) {
    pthread_mutex_lock(&self->mutex);
    self->active = false;
    pthread_mutex_unlock(&self->mutex);
    int left = atomic_fetch_sub(&shared->active_count, 1) - 1;
    log_message(shared->logger, "Болтун %d отключился (осталось %d)", self->id, left);
}

static void handle_incoming(Shared *shared, Talker *self) {
    while (sem_trywait(&self->incoming_sem) == 0) {
        pthread_mutex_lock(&self->mutex);
        CallRequest req = self->incoming;
        self->incoming.has_request = false;
        pthread_mutex_unlock(&self->mutex);

        Talker *caller = &shared->talkers[req.from_id];
        log_message(shared->logger, "Болтун %d отвечает на звонок %d", self->id, caller->id);
        sem_post(&caller->answer_sem);

        log_message(shared->logger, "Разговор %d ↔ %d (%d мс)", caller->id, self->id, req.duration_ms);
        msleep(req.duration_ms);

        finish_conversation(shared, self, caller->id, req.duration_ms);
    }
}

static bool try_start_call(Shared *shared, Talker *self) {
    const Config *cfg = shared->config;
    int duration = random_range(cfg->min_call_ms, cfg->max_call_ms);

    int attempts = 0;
    while (attempts < cfg->talkers * 2 && !stop_requested() && !timed_out(shared)) {
        int target = random_range(0, cfg->talkers - 1);
        if (target == self->id) { attempts++; continue; }
        Talker *callee = &shared->talkers[target];

        pthread_mutex_lock(&callee->mutex);
        bool available = callee->active && !callee->busy;
        if (available) {
            callee->busy = true;
            callee->incoming.from_id = self->id;
            callee->incoming.duration_ms = duration;
            callee->incoming.has_request = true;
            pthread_mutex_unlock(&callee->mutex);

            pthread_mutex_lock(&self->mutex);
            self->busy = true;
            pthread_mutex_unlock(&self->mutex);

            log_message(shared->logger, "Болтун %d набирает %d", self->id, target);
            sem_post(&callee->incoming_sem);
            sem_wait(&self->answer_sem);
            if (!self->active) {
                release_self(self);
                return false;
            }
            log_message(shared->logger, "Разговор %d ↔ %d (%d мс)", self->id, target, duration);
            msleep(duration);
            finish_conversation(shared, self, target, duration);
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
    Shared *shared = self->shared;
    const Config *cfg = shared->config;

    while (self->active && !stop_requested() && !timed_out(shared)) {
        int pause_ms = random_range(cfg->min_idle_ms, cfg->max_idle_ms);
        msleep(pause_ms);

        handle_incoming(shared, self);
        if (!self->active || stop_requested() || timed_out(shared)) break;

        if (random_range(0, 1) == 0) {
            // предпочитаем дождаться входящих
            handle_incoming(shared, self);
        } else {
            try_start_call(shared, self);
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

int run_semaphore_mode(const Config *config, Logger *logger) {
    Shared shared = { .config = config, .logger = logger };
    shared.active_count = config->talkers;
    clock_gettime(CLOCK_MONOTONIC, &shared.start_ts);
    srand((unsigned)time(NULL));

    for (int i = 0; i < config->talkers; ++i) {
        Talker *t = &shared.talkers[i];
        t->id = i;
        t->shared = &shared;
        t->active = true;
        t->busy = false;
        t->conversations = 0;
        t->incoming.has_request = false;
        pthread_mutex_init(&t->mutex, NULL);
        sem_init(&t->incoming_sem, 0, 0);
        sem_init(&t->answer_sem, 0, 0);
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
        sem_destroy(&shared.talkers[i].incoming_sem);
        sem_destroy(&shared.talkers[i].answer_sem);
    }
    return 0;
}

