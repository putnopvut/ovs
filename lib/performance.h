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

#ifndef PERFORMANCE_H
#define PERFORMANCE_H 1

#include <stdbool.h>

enum performance_units {
    PERF_MS,
    PERF_US,
    PERF_NS,
};

/* Create a new performance measurement.
 * The "units" are not used for any calculations but are printed when
 * statistics are requested.
 */
void performance_create(const char *name, enum performance_units units);

/* Indicate that a performance measurement is beginning. */
bool performance_start_sample(const char *name, unsigned long long ts);

/* Indicate that a performance measurement has ended. The
 * sample will be added to the history of performance
 * measurements for this tracker
 */
bool performance_end_sample(const char *name, unsigned long long ts);

#endif /* performance.h */
