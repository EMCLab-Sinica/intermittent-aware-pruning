#include <stdint.h>
#include <string.h>

#include "intermittent-cnn.h"
#include "op_handlers.h"
#include "common.h"
#include "data.h"
#include "ops.h"
#include "debug.h"

static uint16_t cur_group[16] = { 0 };
static uint8_t grp_index = 0;
static uint16_t group_last_item;

static uint8_t handle_cur_group(void) {
    uint16_t intermediate_values_offset = 0;

    my_printf_debug("Current group: ");

    for (uint8_t i = 0; i < grp_index; i++) {
        uint16_t cur_node_id = cur_group[i];
        my_printf_debug("%d ", cur_node_id);

        /* schedule it */
        Node *cur_node = &(nodes[cur_node_id]);
        my_printf_debug("op_type = %d" NEWLINE, cur_node->op_type);

        int16_t input_id[3];
        ParameterInfo *input[3];
        if (cur_node->inputs_len != expected_inputs_len[cur_node->op_type]) {
            // unexpected input length
            ERROR_OCCURRED();
        }
        for (uint16_t j = 0; j < cur_node->inputs_len; j++) {
            input_id[j] = node_input(cur_node, j);
            my_printf_debug("input_id[%d] = %d ", j, input_id[j]);
            input[j] = &(parameter_info[input_id[j]]);
            // dump_params(input[j]);
        }

        /* Allocate an ParameterInfo for output. Details are filled by
         * individual operation handlers */
        ParameterInfo *output = &(parameter_info[cur_node_id + model->n_input]);
        output->params_offset = intermediate_values_offset;

        uint32_t new_intermediate_values_offset = (uint32_t)(
            /* use uint32_t here to avoid overflow */
            intermediate_values_offset + output->params_len
        );
        if (new_intermediate_values_offset >= INTERMEDIATE_VALUES_SIZE) {
            /* TODO: reuse the ring buffer */
            // too many immediate values
            ERROR_OCCURRED();
        }

        uint8_t ret = handlers[cur_node->op_type](input, output, &model->extra_data, cur_node->flags);

        (*counter_idx)++;
        if (*counter_idx >= COUNTERS_LEN) {
            ERROR_OCCURRED();
        }

        if (ret != 0) {
            return 1;
        }
        intermediate_values_offset = (uint16_t)new_intermediate_values_offset;

        my_printf_debug("output dims: ");
        uint8_t has_dims = 0;
        for (uint8_t j = 0; j < 4; j++) {
            if (output->dims[j]) {
                has_dims = 1;
                my_printf_debug("%d, ", output->dims[j]);
            }
        }
        my_printf_debug(NEWLINE);
        if (!has_dims) {
            // missing dims
            ERROR_OCCURRED();
        }
        if (get_param_bitwidth(output) == 0) {
            // invalid bitwidth
            ERROR_OCCURRED();
        }

        if (cur_node_id == model->nodes_len - 1) {
            model->running = 0;
            model->run_counter++;
        }
        cur_node->scheduled = 1;
    }
    my_printf_debug(" - %d element(s)." NEWLINE, grp_index);
    return 0;
}

void init_pointers(void) {
    model = (Model*)model_data;
    inputs = (uint16_t*)inputs_data;
    parameters = (uint16_t*)parameters_data;

    nodes = (Node*)(model + 1);
    parameter_info = (ParameterInfo*)(nodes + model->nodes_len);

    my_printf_debug("model->n_input = %d" NEWLINE, model->n_input);
}

int run_model(int8_t *ansptr) {
    grp_index = 0;

    if (!model->running) {
        // reset model
        for (uint16_t i = 0; i < model->nodes_len; i++) {
            Node *cur_node = &(nodes[i]);
            node_input_unmark_all(cur_node);
            cur_node->scheduled = 0;
        }
        *counter_idx = 0;
        model->running = 1;
    }

    power_counters[*counter_idx]++;

    dump_model();

    uint16_t next_node_idx = 0;
    while (next_node_idx < model->nodes_len) {
        for (uint16_t i = next_node_idx; i < model->nodes_len; i++) {
            Node *cur_node = &(nodes[i]);
            uint8_t no_inputs = 1;

            if (cur_node->scheduled) {
                continue;
            }

            for (uint16_t j = 0; j < cur_node->inputs_len; j++) {
                if (node_input(cur_node, j) >= model->n_input && !node_input_marked(cur_node, j)) {
                    no_inputs = 0;
                }
            }
            if (no_inputs) {
                my_printf_debug("Node %d has no inputs." NEWLINE, i);
                cur_group[grp_index] = i;
                grp_index++;
                /* https://stackoverflow.com/a/47417220 */
                next_node_idx = (uint16_t)(i + 1);
                if (grp_index == 16) {
                    break;
                }
            }
        }

        if (!grp_index) {
            // unable to establish a group
            ERROR_OCCURRED();
        }

        if (grp_index < 16) {
            next_node_idx = 0;
        }

        if (handle_cur_group() != 0) {
            return 1;
        }

        group_last_item = cur_group[grp_index - 1];

        if (group_last_item == model->nodes_len - 1) {
            break;
        }

        /**
         * topological sort: remove handled (scheduled) dependent nodes
         */
        for (uint16_t i = cur_group[0]; i < model->nodes_len; i++) {
            Node *cur_node = &(nodes[i]);
            for (uint16_t j = 0; j < cur_node->inputs_len; j++) {
                for (uint8_t k = 0; k < grp_index; k++) {
                    if (node_input(cur_node, j) == cur_group[k] + model->n_input) {
                        node_input_mark(cur_node, j);
                    }
                }
            }
        }

        grp_index = 0;
        memset(cur_group, 0, sizeof(cur_group));

        dump_model();
    }

    /* XXX: is the last node always the output node? */
    ParameterInfo *output_node = &(parameter_info[model->nodes_len + model->n_input - 1]);
    if (ansptr) {
        int16_t max = INT16_MIN;
        for (uint16_t i = 0; i < output_node->dims[1]; i++) {
            int16_t val = *get_q15_param(output_node, i);
            if (val > max) {
                *ansptr = (uint8_t)i;
                max = val;
            }
        }
    }

    return 0;
}

void print_results(void) {
    ParameterInfo *output_node = &(parameter_info[model->nodes_len + model->n_input - 1]);
    for (uint16_t i = 0; i < output_node->dims[1]; i++) {
        print_q15(*get_q15_param(output_node, i));
    }
    my_printf(NEWLINE);

    my_printf("ticks: ");
    for (uint8_t i = 0; i < *counter_idx; i++) {
        my_printf("%d ", counters[i]);
    }
    my_printf(NEWLINE "power counters: ");
    for (uint8_t i = 0; i < *counter_idx; i++) {
        my_printf("%d ", power_counters[i]);
    }
    my_printf(NEWLINE "run_counter: %d", model->run_counter);
    my_printf(NEWLINE);
}
