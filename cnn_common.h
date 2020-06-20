#pragma once

#include <stddef.h> /* size_t, see https://stackoverflow.com/a/26413264 */
#include <stdint.h>
#include "data.h"

//#define WITH_PROGRESS_EMBEDDING

#define NUM_FILTERS 16

/**********************************
 *        Data structures         *
 **********************************/
typedef struct Node {
    uint16_t inputs_len;
    uint16_t inputs_offset;
    uint16_t max_output_id;
    uint16_t op_type;
    /* Layout of 16 bits in flags
     * 15-08 generic flags
     * 07-04 kernel_size (used in MaxPool)
     * 03-00 stride (used in Conv and MaxPool)
     **/
    uint16_t flags;
    uint16_t scheduled;  /* 16 bits for aligned memory */
} Node;

// _Static_assert in C11 requires the message
// We target C99 - it works, anyway
_Static_assert(sizeof(Node) == 12, "Unexpected size for Node");

/* ParameterInfo may indicate data from the model (parameters) or intermediate values */
typedef struct ParameterInfo {
    uint32_t params_offset;
    uint32_t params_len;  /* in bytes */
    /* Known bitwidth values:
     * 16: q15
     * 32: iq31
     * 64: INT64 (from ONNX)
     */
    uint8_t bitwidth;
    /* A flag to indicate where the data are. Possible values are SLOT_TEST_SET,
     * SLOT_PARAMETERS and SLOT_INTERMEDIATE_VALUES.
     */
    uint8_t slot;
    uint16_t dummy;
    // uint8_t is not enough. For example, fully connected layer in MNIST has dims 256x1
    uint16_t dims[4];
} ParameterInfo;

_Static_assert(sizeof(ParameterInfo) == 20, "Unexpected size for ParameterInfo");

typedef struct Model {
    uint16_t nodes_len;
    uint16_t n_input;
    uint16_t running;
    uint16_t recovery;
    uint16_t run_counter;
    uint16_t state_bit;
    uint16_t sample_idx;
} Model;

_Static_assert(sizeof(Model) == 14, "Unexpected size for Model");

typedef struct {
    uint16_t time_counters[COUNTERS_LEN];
    uint16_t power_counters[COUNTERS_LEN];
    uint16_t dma_invocations[COUNTERS_LEN];
    uint16_t dma_bytes[COUNTERS_LEN];
    uint16_t counter_idx;
} Counters;

// Keep the following coefficients synced with transform.py
_Static_assert(sizeof(Counters) == 8 * COUNTERS_LEN + 2, "Unexpected size of Counters");

/**********************************
 *          Global data           *
 **********************************/
extern uint8_t *inputs_data;
Counters *counters(void);


/**********************************
 *          Miscellaneous         *
 **********************************/

/* MSP430 SDK already defines MIN, which means minutes */
#define MIN_VAL(x, y) ((x) < (y) ? (x) : (y))
#define MAX_VAL(x, y) ((x) > (y) ? (x) : (y))

/* Better to not use macros
 * https://stackoverflow.com/a/3437484/3786245
 */
static inline int16_t int16_min(int16_t a, int16_t b) {
    return a < b ? a : b;
}

static inline int16_t int16_max(int16_t a, int16_t b) {
    return a > b ? a : b;
}

#define UNUSED(x) (void)(x)

/**********************************
 *       Helpers for nodes        *
 **********************************/
int16_t* get_q15_param(ParameterInfo *param, size_t i);
int32_t* get_iq31_param(ParameterInfo *param, size_t i);
int64_t get_int64_param(ParameterInfo *param, size_t i);
int16_t node_input(Node *node, size_t i);
static inline int16_t iq31_to_q15(int32_t val) {
    return (int16_t)(val >> 16);
}

/**********************************
 *       Operation handlers       *
 **********************************/
typedef void (*handler)(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags);
typedef uint32_t (*allocator)(ParameterInfo *input[], ParameterInfo *output, uint16_t flags);
// below are defined in ops.c
extern uint8_t expected_inputs_len[];
extern uint8_t inplace_update[];
extern handler handlers[];
extern allocator allocators[];
