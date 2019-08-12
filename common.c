#include <stdio.h>

#include "common.h"

static int16_t* node_input_ptr(Node *node, size_t i) {
    return (int16_t*)((uint8_t*)inputs + node->inputs_offset) + i;
}

int16_t node_input(Node *node, size_t i) {
    return *node_input_ptr(node, i) / 2;
}

void node_input_mark(Node *node, size_t i) {
    int16_t *ptr = node_input_ptr(node, i);
    *ptr |= 1;
}

uint8_t node_input_marked(Node *node, size_t i) {
    int16_t *ptr = node_input_ptr(node, i);
    return *ptr & 0x1;
}

static uint8_t* get_param_base_pointer(ParameterInfo *param) {
    if (param->bitwidth_and_flags & FLAG_INTERMEDIATE_VALUES) {
        return &(intermediate_values[0]);
    } else {
        return (uint8_t*)parameters;
    }
}

int16_t* get_q15_param(ParameterInfo *param, size_t i) {
    if ((param->bitwidth_and_flags >> 1) != 16) {
        printf("Error: incorrect param passed to %s" NEWLINE, __func__);
        return NULL;
    }
    return (int16_t*)(get_param_base_pointer(param) + param->params_offset) + i;
}

int32_t* get_iq31_param(ParameterInfo *param, size_t i) {
    if ((param->bitwidth_and_flags >> 1) != 32) {
        printf("Error: incorrect param passed to %s" NEWLINE, __func__);
        return NULL;
    }
    return (int32_t*)(get_param_base_pointer(param) + param->params_offset) + i;
}

int64_t get_int64_param(ParameterInfo *param, size_t i) {
    return *((int64_t*)((uint8_t*)parameters + param->params_offset) + i);
}

