/*
 * Copyright (c) 2018 eBay Inc.
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

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "openvswitch/dynamic-string.h"
#include "openvswitch/hmap.h"
#include "openvswitch/vlog.h"
#include "inc-proc-eng.h"

VLOG_DEFINE_THIS_MODULE(inc_proc_eng);

bool engine_force_recompute = false;

void
engine_run(struct engine_node *node, uint64_t run_id)
{
    if (node->run_id == run_id) {
        return;
    }
    node->run_id = run_id;

    if (node->changed) {
        node->changed = false;
    }
    if (!node->n_inputs) {
        node->run(node);
        VLOG_DBG("node: %s, changed: %d", node->name, node->changed);
        return;
    }

    size_t i;

    for (i = 0; i < node->n_inputs; i++) {
        engine_run(node->inputs[i].node, run_id);
    }

    bool need_compute = false;
    bool need_recompute = false;

    if (engine_force_recompute) {
        need_recompute = true;
    } else {
        for (i = 0; i < node->n_inputs; i++) {
            if (node->inputs[i].node->changed) {
                need_compute = true;
                if (!node->inputs[i].change_handler) {
                    need_recompute = true;
                    break;
                }
            }
        }
    }

    if (need_recompute) {
        VLOG_DBG("node: %s, recompute (%s)", node->name,
                 engine_force_recompute ? "forced" : "triggered");
        node->run(node);
    } else if (need_compute) {
        for (i = 0; i < node->n_inputs; i++) {
            if (node->inputs[i].node->changed) {
                VLOG_DBG("node: %s, handle change for input %s",
                         node->name, node->inputs[i].node->name);
                if (!node->inputs[i].change_handler(node)) {
                    VLOG_DBG("node: %s, can't handle change for input %s, "
                             "fall back to recompute",
                             node->name, node->inputs[i].node->name);
                    node->run(node);
                    break;
                }
            }
        }
    }

    VLOG_DBG("node: %s, changed: %d", node->name, node->changed);

}

