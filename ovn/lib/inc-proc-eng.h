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

#ifndef INC_PROC_ENG_H
#define INC_PROC_ENG_H 1

// TODO: add documentation of incremental processing engine.

#define ENGINE_MAX_INPUT 256

struct engine_node;

struct engine_node_input {
    struct engine_node *node;
    /* change_handler handles one input change against "old_data" of all
     * other inputs, returns:
     *  - true: if change can be handled
     *  - false: if change cannot be handled (suggesting full recompute)
     */
    bool (*change_handler)(struct engine_node *node);
};

struct engine_node {
    uint64_t run_id;
    char* name;
    size_t n_inputs;
    struct engine_node_input inputs[ENGINE_MAX_INPUT];
    void *data;
    bool changed;
    void *context;
    void (*run)(struct engine_node *node);
};

void
engine_run(struct engine_node *node, uint64_t run_id);

bool
engine_need_run(struct engine_node *node);

static inline struct engine_node *
engine_get_input(const char *input_name, struct engine_node *node)
{
    size_t i;
    for (i = 0; i < node->n_inputs; i++) {
        if (!strcmp(node->inputs[i].node->name, input_name)) {
            return node->inputs[i].node;
        }
    }
    return NULL;
}

static inline void
engine_add_input(struct engine_node *node, struct engine_node *input,
    bool (*change_handler)(struct engine_node *node))
{
    node->inputs[node->n_inputs].node = input;
    node->inputs[node->n_inputs].change_handler = change_handler;
    node->n_inputs ++;
}

extern bool engine_force_recompute;
static inline void
engine_set_force_recompute(bool val)
{
    engine_force_recompute = val;
}

#define ENGINE_NODE(NAME, NAME_STR) \
    struct engine_node en_##NAME = { \
        .name = NAME_STR, \
        .data = &ed_##NAME, \
        .context = &ctx, \
        .run = NAME##_run, \
    };

#define ENGINE_FUNC_OVSDB(DB_NAME, TBL_NAME, IDL) \
static void \
DB_NAME##_##TBL_NAME##_run(struct engine_node *node) \
{ \
    static bool first_run = true; \
    if (first_run) { \
        first_run = false; \
        node->changed = true; \
        return; \
    } \
    struct controller_ctx *ctx = (struct controller_ctx *)node->context; \
    if (DB_NAME##rec_##TBL_NAME##_track_get_first(IDL)) { \
        node->changed = true; \
        return; \
    } \
    node->changed = false; \
}

#define ENGINE_FUNC_SB(TBL_NAME) \
    ENGINE_FUNC_OVSDB(sb, TBL_NAME, ctx->ovnsb_idl)

#define ENGINE_FUNC_OVS(TBL_NAME) \
    ENGINE_FUNC_OVSDB(ovs, TBL_NAME, ctx->ovs_idl)

#define ENGINE_NODE_SB(TBL_NAME, TBL_NAME_STR) \
    void *ed_sb_##TBL_NAME; \
    ENGINE_NODE(sb_##TBL_NAME, TBL_NAME_STR)

#define ENGINE_NODE_OVS(TBL_NAME, TBL_NAME_STR) \
    void *ed_ovs_##TBL_NAME; \
    ENGINE_NODE(ovs_##TBL_NAME, TBL_NAME_STR)

#endif /* ovn/lib/inc-proc-eng.h */
