/* Copyright (c) 2017 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "performance.h"
#include "timer.h"
#include "timeval.h"
#include "openvswitch/shash.h"
#include "openvswitch/vlog.h"
#include "unixctl.h"
#include "openvswitch/dynamic-string.h"
#include "openvswitch/poll-loop.h"
#include "ovs-thread.h"
#include <unistd.h>
#include "socket-util.h"

VLOG_DEFINE_THIS_MODULE(performance);

struct sample {
    unsigned long long start_time; /* Time when we started this sample */
    unsigned long long end_time;   /* Time when we ended this sample */
    unsigned long long elapsed;    /* Elapsed time: end_time - start_time */
};

struct sample_vec {
    struct sample *samples; /* Dynamic array of samples */
    size_t n_samples;       /* Number of samples */
    size_t capacity;        /* Number of allocated samples */
};

struct stats {
    unsigned long long min;         /* Minimum measurement (ms) */
    unsigned long long max;         /* Maximum measurement (ms) */
    double average;                 /* Average measurement (ms) */
    unsigned long long percentile;  /* 95th percentile measurement (ms) */
    unsigned long long num_samples; /* Total number of measurements */
};

struct performance {
    struct sample_vec vec;
    struct timer timer;
    struct sample cur_sample;
    enum performance_units units;
};

static struct shash performances = SHASH_INITIALIZER(&performances);
static struct ovs_mutex performances_lock = OVS_MUTEX_INITIALIZER;

static int performance_pipe[2];
static pthread_t performance_thread_id;

#define PERFORMANCE_INTERVAL 10000

const unsigned long long performance_oldest[] = {
    [PERF_MS] = 600000llu,
    [PERF_US] = 600000000llu,
    [PERF_NS] = 600000000000llu,
};

const unsigned long long one_min[] = {
    [PERF_MS] = 60000llu,
    [PERF_US] = 60000000llu,
    [PERF_NS] = 60000000000llu,
};

const unsigned long long five_min[] = {
    [PERF_MS] = 300000llu,
    [PERF_US] = 300000000llu,
    [PERF_NS] = 300000000000llu,
};

const unsigned long long ten_min[] = {
    [PERF_MS] = 600000llu,
    [PERF_US] = 600000000llu,
    [PERF_NS] = 600000000000llu,
};

const char *unit_name[] = {
    [PERF_MS] = "msec",
    [PERF_US] = "usec",
    [PERF_NS] = "nsec",
};

static void
add_sample(struct sample_vec *vec, struct sample *new_sample)
{
    if (vec->capacity == vec->n_samples) {
        vec->samples = x2nrealloc(vec->samples, &vec->capacity,
            sizeof *vec->samples);
    }

    vec->samples[vec->n_samples++] = *new_sample;
}

static int
find_earliest(const struct sample_vec *vec, unsigned long long age)
{
    unsigned long long cutoff = time_msec() - age;

    for (int i = 0; i < vec->n_samples; i++) {
        if (vec->samples[i].end_time >= cutoff) {
            return i;
        }
    }

    /* Either the vector is empty or all times are
     * older than the cutoff.
     */
    return -1;
}

static double
average(const struct sample *samples, size_t num_samples)
{
    /* Avoid division by zero */
    if (num_samples == 0) {
        return 0;
    }

    long long int sum = 0;
    for (size_t i = 0; i < num_samples; i++) {
        sum += samples[i].elapsed;
    }

    return sum / (double) num_samples;
}

/* Calculate a specific percentile value.
 * Valid percentile values range from 0 to 99
 */
static long long int
percentile(const struct sample *samples, size_t num_samples, int percentile)
{
    if (num_samples == 0) {
        return 0;
    }

    size_t pctl = num_samples * percentile / 100;
    return samples[pctl].elapsed;
}

static void
cull_old_times(struct sample_vec *vec, unsigned long long int age)
{
    int i = find_earliest(vec, age);

    if (i <= 0) {
        return;
    }

    size_t new_size = vec->n_samples - i;
    memmove(vec->samples, &vec->samples[i], new_size * sizeof *vec->samples);
    vec->n_samples = new_size;
}

static void
format_stats(struct ds *s, const char *prefix, const struct stats *stats,
             const char *units)
{
    if (stats->num_samples) {
        ds_put_format(s, "\t%s samples: %llu\n", prefix,
                      stats->num_samples);
        ds_put_format(s, "\t%s minimum: %llu %s\n", prefix,
                      stats->min, units);
        ds_put_format(s, "\t%s maximum: %llu %s\n", prefix,
                      stats->max, units);
        ds_put_format(s, "\t%s average: %.3f %s\n", prefix,
                      stats->average, units);
        ds_put_format(s, "\t%s 95th percentile: %llu %s\n\n", prefix,
                      stats->percentile, units);
    } else {
        ds_put_format(s, "\t%s samples: 0\n", prefix);
        ds_put_format(s, "\t%s minimum: N/A\n", prefix);
        ds_put_format(s, "\t%s maximum: N/A\n", prefix);
        ds_put_format(s, "\t%s average: N/A\n", prefix);
        ds_put_format(s, "\t%s 95th percentile: N/A\n\n", prefix);
    }
}

static int
cmp_times(const void *left_, const void *right_)
{
    const struct sample *left = left_;
    const struct sample *right = right_;

    return left->elapsed == right->elapsed ? 0
        : left->elapsed > right->elapsed;
}

static struct sample *
sorted_copy_times(const struct sample *by_time, size_t vec_size)
{
    struct sample *copy = xmemdup(by_time, vec_size * sizeof *copy);
    qsort(copy, vec_size, sizeof *copy, cmp_times);

    return copy;
}

static void
get_stats(const struct sample_vec *vec, long long int age_ms,
          struct stats *stats)
{
    int start_idx = find_earliest(vec, age_ms);
    if (start_idx < 0) {
        memset(stats, 0, sizeof *stats);
        return;
    }
    size_t vec_size = vec->n_samples - start_idx;

    struct sample *by_time = &vec->samples[start_idx];
    struct sample *by_elapsed = sorted_copy_times(by_time, vec_size);

    stats->min = by_elapsed[0].elapsed;
    stats->max = by_elapsed[vec_size - 1].elapsed;
    stats->average = average(by_time, vec_size);
    stats->percentile = percentile(by_elapsed, vec_size, 95);
    stats->num_samples = vec_size;

    free(by_elapsed);
}

static void
performance_print(struct performance *perf, const char *name,
                  struct ds *s)
{
    struct stats one_min_stats;
    struct stats five_min_stats;
    struct stats ten_min_stats;
    ds_put_format(s, "Statistics for '%s'\n", name);

    get_stats(&perf->vec, one_min[perf->units], &one_min_stats);
    get_stats(&perf->vec, five_min[perf->units], &five_min_stats);
    get_stats(&perf->vec, ten_min[perf->units], &ten_min_stats);

    const char *units = unit_name[perf->units];
    format_stats(s, "1 minute", &one_min_stats, units);
    format_stats(s, "5 minute", &five_min_stats, units);
    format_stats(s, "10 minute", &ten_min_stats, units);
}

static bool
performance_show_protected(int argc, const char *argv[], struct ds *s)
{
    struct performance *perf;

    if (argc > 1) {
        perf = shash_find_data(&performances, argv[1]);
        if (!perf) {
            ds_put_cstr(s, "No such performance");
            return false;
        }
        performance_print(perf, argv[1], s);
    } else {
        struct shash_node *node;
        SHASH_FOR_EACH (node, &performances) {
            perf = node->data;
            performance_print(perf, node->name, s);
        }
    }

    return true;
}

static void
performance_show(struct unixctl_conn *conn, int argc OVS_UNUSED,
        const char *argv[], void *ignore OVS_UNUSED)
{
    struct ds s = DS_EMPTY_INITIALIZER;
    bool success;

    ovs_mutex_lock(&performances_lock);
    success = performance_show_protected(argc, argv, &s);
    ovs_mutex_unlock(&performances_lock);

    if (success) {
        unixctl_command_reply(conn, ds_cstr(&s));
    } else {
        unixctl_command_reply_error(conn, ds_cstr(&s));
    }
    ds_destroy(&s);
}

enum performance_op {
    OP_START_SAMPLE,
    OP_END_SAMPLE,
    OP_SHUTDOWN,
};

struct performance_packet {
    enum performance_op op;
    /* XXX Arbitrary size? */
    char name[32];
    unsigned long long time;
};

static bool
performance_start_sample_protected(const struct performance_packet *pkt)
{
    struct performance *perf = shash_find_data(&performances, pkt->name);
    if (!perf) {
        return false;
    }

    /* We already started sampling. Need an end before
     * we start another sample
     */
    if (perf->cur_sample.start_time) {
        return false;
    }

    perf->cur_sample.start_time = pkt->time;
    return true;
}

static bool
performance_end_sample_protected(const struct performance_packet *pkt)
{
    struct performance *perf = shash_find_data(&performances, pkt->name);
    if (!perf) {
        return false;
    }

    /* We can't end a sample if we haven't started one */
    if (!perf->cur_sample.start_time) {
        return false;
    }

    perf->cur_sample.end_time = pkt->time;
    perf->cur_sample.elapsed = perf->cur_sample.end_time
        - perf->cur_sample.start_time;

    add_sample(&perf->vec, &perf->cur_sample);

    memset(&perf->cur_sample, 0, sizeof perf->cur_sample);
    return true;
}

static void *
performance_thread(void *ign OVS_UNUSED)
{
    struct timer timer;
    bool should_exit = false;

    timer_set_duration(&timer, PERFORMANCE_INTERVAL);

    while (!should_exit) {
        if (timer_expired(&timer)) {
            struct shash_node *node;
            ovs_mutex_lock(&performances_lock);
            SHASH_FOR_EACH (node, &performances) {
                struct performance *perf = node->data;
                cull_old_times(&perf->vec, performance_oldest[perf->units]);
            }
            ovs_mutex_unlock(&performances_lock);
            timer_set_duration(&timer, PERFORMANCE_INTERVAL);
        }

        struct performance_packet pkt;
        if (read(performance_pipe[0], &pkt, sizeof(pkt)) > 0) {
            ovs_mutex_lock(&performances_lock);
            switch (pkt.op) {
            case OP_START_SAMPLE:
                performance_start_sample_protected(&pkt);
                break;
            case OP_END_SAMPLE:
                performance_end_sample_protected(&pkt);
                break;
            case OP_SHUTDOWN:
                should_exit = true;
                break;
            }
            ovs_mutex_unlock(&performances_lock);
        }

        if (!should_exit) {
            timer_wait(&timer);
            poll_fd_wait(performance_pipe[0], POLLIN);
            poll_block();
        }
    }

    return NULL;
}

static void
performance_exit(void)
{
    struct shash_node *node, *node_next;
    struct performance_packet pkt = {
        .op = OP_SHUTDOWN,
    };

    write(performance_pipe[1], &pkt, sizeof pkt);
    xpthread_join(performance_thread_id, NULL);

    /* Process is exiting and we have joined the only
     * other competing thread. We are now the sole owners
     * of all data in the file.
     */
    SHASH_FOR_EACH_SAFE (node, node_next, &performances) {
        struct performance *perf = node->data;
        shash_delete(&performances, node);
        free(perf->vec.samples);
        free(perf);
    }
    shash_destroy(&performances);
    ovs_mutex_destroy(&performances_lock);
}

static void
do_init_performance(void)
{
    unixctl_command_register("performance/show", "[NAME]", 0, 1,
                             performance_show, NULL);
    xpipe_nonblocking(performance_pipe);
    performance_thread_id = ovs_thread_create(
        "performance", performance_thread, NULL);
    atexit(performance_exit);
}

static void
performance_init(void)
{
    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;
    if (ovsthread_once_start(&once)) {
        do_init_performance();
        ovsthread_once_done(&once);
    }
}

void
performance_create(const char *name, enum performance_units units)
{
    performance_init();

    struct performance *perf = xzalloc(sizeof *perf);
    perf->units = units;

    ovs_mutex_lock(&performances_lock);
    shash_add(&performances, name, perf);
    ovs_mutex_unlock(&performances_lock);
}

bool
performance_start_sample(const char *name, unsigned long long ts)
{
    struct performance_packet pkt = {
        .op = OP_START_SAMPLE,
        .time = ts,
    };
    ovs_strlcpy(pkt.name, name, sizeof(pkt.name));
    write(performance_pipe[1], &pkt, sizeof(pkt));

    return true;
}

bool
performance_end_sample(const char *name, unsigned long long ts)
{
    struct performance_packet pkt = {
        .op = OP_END_SAMPLE,
        .time = ts,
    };
    ovs_strlcpy(pkt.name, name, sizeof(pkt.name));
    write(performance_pipe[1], &pkt, sizeof(pkt));

    return true;
}
