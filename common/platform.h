#pragma once

#include <stdint.h>
#include <stdlib.h>

#if defined(__MSP430__) || defined(__MSP432__)
#  include "plat-msp430.h"
#else
#  include "plat-linux.h"
#endif

struct ParameterInfo;
struct Model;

#ifdef __cplusplus
extern "C" {
#endif
[[ noreturn ]] void ERROR_OCCURRED(void);
#ifdef __cplusplus
}
#endif
void my_memcpy(void* dest, const void* src, size_t n);
void my_memcpy_to_param(struct ParameterInfo *param, uint16_t offset_in_word, const void *src, size_t n);
void my_memcpy_from_intermediate_values(void *dest, const struct ParameterInfo *param, uint16_t offset_in_word, size_t n);
void read_from_samples(void *dest, uint16_t offset_in_word, size_t n);
struct ParameterInfo* get_intermediate_parameter_info(uint8_t i);
void commit_intermediate_parameter_info(uint8_t i);
struct Model* get_model(void);
void commit_model(void);
uint16_t read_max_multiplier(const struct ParameterInfo* param);
void write_max_multiplier(const struct ParameterInfo* param, uint16_t max_multiplier);
void first_run(void);
#if HAWAII
void write_hawaii_layer_footprint(uint16_t layer_idx, uint16_t n_jobs);
uint16_t read_hawaii_layer_footprint(uint16_t layer_idx);
void reset_hawaii_layer_footprint(uint16_t layer_idx);
#endif
