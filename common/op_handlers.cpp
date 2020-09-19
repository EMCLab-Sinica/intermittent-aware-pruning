#include "cnn_common.h"
#include "op_handlers.h"
#include "my_debug.h"
#include "platform.h"
#include "conv.h"
#include "intermittent-cnn.h"
#include "my_dsplib.h"

// Not using DSPLIB_DATA here as it does not work under C++ (?)
#ifdef __MSP430__
#pragma DATA_SECTION(".leaRAM")
#endif
int16_t lea_buffer[LEA_BUFFER_SIZE];

#define RESHAPE_AUTO_DIM (uint16_t)(-1)

#if STATEFUL_CNN
void find_initial_state_bit(int16_t* p_offset, uint8_t* p_turning_point_idx, int16_t* p_next_turning_point, SlotInfo** p_slot_info, uint32_t initial_value_idx, Model* model, ParameterInfo* param) {
    *p_offset = get_state_bit(model, param->slot) ? 0x4000 : 0;
    *p_turning_point_idx = 0;
    *p_next_turning_point = -1;
    *p_slot_info = get_slot_info(model, param->slot);
    uint8_t next_turning_point_found = 0;
    if (!(*p_slot_info)) {
        return;
    }
    while (*p_turning_point_idx < (*p_slot_info)->n_turning_points) {
        *p_next_turning_point = (*p_slot_info)->turning_points[*p_turning_point_idx];
        (*p_turning_point_idx)++;
        if (*p_next_turning_point > static_cast<int16_t>(initial_value_idx)) {
            next_turning_point_found = 1;
            break;
        }
        *p_offset ^= 0x4000;
    }
    if (!next_turning_point_found) {
        *p_next_turning_point = -1;
    }
}

void check_next_turning_point_inner(int16_t* p_offset, uint8_t* p_turning_point_idx, int16_t* p_next_turning_point, SlotInfo* slot_info, uint16_t value_idx) {
    *p_offset ^= 0x4000;
    uint8_t next_turning_point_found = 0;
    while (*p_turning_point_idx < slot_info->n_turning_points) {
        *p_next_turning_point = slot_info->turning_points[*p_turning_point_idx];
        (*p_turning_point_idx)++;
        if (*p_next_turning_point >= value_idx) {
            next_turning_point_found = 1;
            break;
        }
        *p_offset ^= 0x4000;
    }
    if (!next_turning_point_found) {
        *p_next_turning_point = -1;
    }
}
#endif

void alloc_maxpool(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags* flags) {
    uint16_t stride = flags->stride;

    ParameterInfo *data = input[0];

    const uint16_t CHANNEL = data->dims[1], H = data->dims[2], W = data->dims[3];
    uint16_t new_H = H / stride;
    uint16_t new_W = W / stride;

    output->params_len = new_H * new_W * CHANNEL * sizeof(int16_t);
    output->slot = get_next_slot(model, data);
    output->dims[0] = 1;
    output->dims[1] = CHANNEL;
    output->dims[2] = new_H;
    output->dims[3] = new_W;
}

static int16_t maxpool_patch(uint16_t output_h, uint16_t output_w, uint16_t c, NodeFlags* flags, ParameterInfo *data, Model *model) {
    const uint16_t CHANNEL = data->dims[1], W = data->dims[3];
    uint16_t stride = flags->stride;
    uint16_t kernel_size = flags->kernel_size;

    int16_t offset_h, offset_w;
    offset_h = W * CHANNEL;
    offset_w = CHANNEL;

    my_printf_debug("output_h=% 3d ", output_h);
    my_printf_debug("output_w=% 3d ", output_w);
    my_printf_debug("c=% 3d ", c);

    int16_t max_val = INT16_MIN;
    for (uint16_t sH = 0; sH < kernel_size; sH++) {
        for (uint16_t sW = 0; sW < kernel_size; sW++) {
            uint16_t val_offset = (output_h*stride+sH) * offset_h + (output_w*stride+sW) * offset_w + c;
            int16_t val = get_q15_param(data, val_offset);
#if STATEFUL_CNN
            if (get_value_state_bit(val)) {
                // assuming input state bits are correct...
                val -= 0x4000;
            }
#endif
            // dump_value_debug(model, data, val_offset);
            my_printf_debug("% 5d ", val);
            // XXX: use LEA?
            if (val > max_val) {
                max_val = val;
            }
        }
    }
    // need a space as dump_value does not append spaces when DUMP_INTEGERS is not defined
    my_printf_debug(" max=% 5d ", max_val);
    return max_val;
}

void handle_maxpool(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags* flags) {
    my_printf_debug("MaxPool!" NEWLINE);

    uint16_t stride = flags->stride;
    uint8_t need_nhwc2nchw = (flags->generic == NHWC2NCHW);

    /* XXX: add flags; assume no padding for now */
    ParameterInfo *data = input[0];

    my_printf_debug("handle_maxpool input" NEWLINE);
    dump_params_debug(model, data);

    const uint16_t CHANNEL = data->dims[1], H = data->dims[2], W = data->dims[3];
    uint16_t new_H = H / stride;
    uint16_t new_W = W / stride;

    determine_tile_c(output);
    uint16_t tile_c = output->tile_c;
    my_printf_debug("tile_c = %d" NEWLINE, tile_c);

    uint16_t tile_c_offset = 0;

    uint16_t output_h = 0, output_w = 0, c = 0;
    uint16_t output_offset = 0;

#if STATEFUL_CNN
    uint32_t first_unfinished_value_offset = recovery_from_state_bits(model, output);
    uint16_t initial_n, initial_c, initial_h, initial_w;
    initial_n = first_unfinished_value_offset / (new_H * new_W * tile_c);

    tile_c_offset = initial_n * tile_c;

    int16_t offset, next_output_turning_point;
    uint8_t output_turning_point_idx;
    SlotInfo *output_slot_info;
    find_initial_state_bit(&offset, &output_turning_point_idx, &next_output_turning_point, &output_slot_info, first_unfinished_value_offset, model, output);
    offset ^= 0x4000;

    uint16_t initial_real_tile_c = MIN_VAL(tile_c, CHANNEL - tile_c_offset);
    output_offset = first_unfinished_value_offset;
    if (!need_nhwc2nchw) {
        initial_c = first_unfinished_value_offset % initial_real_tile_c;
        first_unfinished_value_offset /= initial_real_tile_c;
        initial_w = first_unfinished_value_offset % new_W;
        first_unfinished_value_offset /= new_W;
        initial_h = first_unfinished_value_offset % new_H;
    } else {
        initial_w = first_unfinished_value_offset % new_W;
        first_unfinished_value_offset /= new_W;
        initial_h = first_unfinished_value_offset % new_H;
        first_unfinished_value_offset /= new_H;
        initial_c = first_unfinished_value_offset % initial_real_tile_c;
    }
    output_h = initial_h;
    output_w = initial_w;
    c = initial_c;
    my_printf_debug("initial_n = %d" NEWLINE, initial_n);
    my_printf_debug("initial_h = %d" NEWLINE, initial_h);
    my_printf_debug("initial_w = %d" NEWLINE, initial_w);
    my_printf_debug("initial_c = %d" NEWLINE, initial_c);
#endif

    for (; tile_c_offset < CHANNEL; tile_c_offset += tile_c) {
        uint16_t real_tile_c = MIN_VAL(tile_c, CHANNEL - tile_c_offset);
        if (!need_nhwc2nchw) {
            // NHWC
            for (; output_h < new_H; output_h++) {
                for (; output_w < new_W; output_w++) {
                    for (; c < real_tile_c; c++) {
                        int16_t max_val = maxpool_patch(output_h, output_w, c + tile_c_offset, flags, data, model);
                        my_printf_debug("output_offset=%d" NEWLINE, output_offset);
#if STATEFUL_CNN
                        check_next_turning_point(offset, output_turning_point_idx, next_output_turning_point, output_slot_info, output_offset);
                        max_val += offset;
#endif
                        put_q15_param(output, output_offset, max_val);
                        output_offset++;
                    }
                    c = 0;
                }
                output_w = 0;
            }
            output_h = 0;
        } else {
            // NCHW
            for (; c < real_tile_c; c++) {
                for (; output_h < new_H; output_h++) {
                    for (; output_w < new_W; output_w++) {
                        int16_t max_val = maxpool_patch(output_h, output_w, c + tile_c_offset, flags, data, model);
                        my_printf_debug("output_offset=%d" NEWLINE, output_offset);
#if STATEFUL_CNN
                        check_next_turning_point(offset, output_turning_point_idx, next_output_turning_point, output_slot_info, output_offset);
                        max_val += offset;
#endif
                        put_q15_param(output, output_offset, max_val);
                        output_offset++;
                    }
                    output_w = 0;
                }
                output_h = 0;
            }
            c = 0;
        }
    }

    MY_ASSERT(output_offset == output->params_len / sizeof(int16_t));

#if STATEFUL_CNN
    flip_state_bit(model, output);
#endif

    my_printf_debug("handle_maxpool output" NEWLINE);
    if (!need_nhwc2nchw) {
        dump_params_nhwc_debug(model, output, 0);
    } else if (tile_c == CHANNEL) {
        dump_params_debug(model, output);
    }
}

void alloc_add(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    ParameterInfo *A = input[0];
    MY_ASSERT(A->bitwidth == 16 && input[1]->bitwidth == 16);

    output->slot = get_next_slot(model, A);
}

class AddOutputChunkHandler : public ChunkHandler {
public:
    AddOutputChunkHandler(int16_t *_buffer) : buffer(_buffer) {}

    void operator () (uint32_t offset, uint16_t real_chunk_len, uint8_t state_bit) const override {
        if (!state_bit) {
            int16_t* to_offset = buffer + offset;
            my_offset_q15(to_offset, 0x4000, to_offset, real_chunk_len);
        }
    }

private:
    int16_t *buffer;
};

void handle_add(Model* model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    /* Add: Y = X + W */
    my_printf_debug("Add!" NEWLINE);

    ParameterInfo *A = input[0], *B = input[1];

    my_printf_debug("handle_add input A" NEWLINE);
    dump_params_debug(model, A);
    my_printf_debug("handle_add input B" NEWLINE);
    dump_params_debug(model, B);

    uint16_t vector_size = A->dims[1];

    int16_t *buffer_a = lea_buffer,
            *buffer_b = lea_buffer + vector_size;
    my_memcpy_from_param(buffer_a, A, 0, output->params_len);
    my_memcpy_from_param(buffer_b, B, 0, output->params_len);

#if STATEFUL_CNN
    // XXX: use LEA?
    for (uint16_t idx = 0; idx < vector_size; idx++) {
        if (get_value_state_bit(buffer_a[idx])) {
            buffer_a[idx] -= 0x4000;
        }
        if (get_value_state_bit(buffer_b[idx])) {
            buffer_a[idx] -= 0x4000;
        }
    }
#endif

    int16_t scaleFract;
    uint8_t shift;
    if (A->scale > B->scale) {
        float_to_scale_params(&scaleFract, &shift, 1.0f * B->scale / A->scale);
        my_scale_q15(buffer_b, scaleFract, shift, buffer_b, vector_size);
    } else if (B->scale > A->scale) {
        float_to_scale_params(&scaleFract, &shift, 1.0f * A->scale / B->scale);
        my_scale_q15(buffer_a, scaleFract, shift, buffer_a, vector_size);
    }
    my_add_q15(buffer_a, buffer_b, buffer_a, vector_size);

#if STATEFUL_CNN
    iterate_chunks(model, output, 0, vector_size, AddOutputChunkHandler(buffer_a));
#endif

    my_memcpy_to_param(output, 0, buffer_a, output->params_len);

#if STATEFUL_CNN
    flip_state_bit(model, output);
#endif

    my_printf_debug("handle_add output" NEWLINE);
    dump_params_debug(model, output);
}

void alloc_matmul(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    ParameterInfo *A = input[0], *B = input[1];

    uint16_t output_len = A->dims[0] * B->dims[1];

    output->dims[0] = A->dims[0];
    output->dims[1] = B->dims[1];
    output->params_len = output_len * sizeof(int16_t);
    output->bitwidth = 16;
    output->slot = get_next_slot(model, A);
    output->scale = A->scale * B->scale;
}

class MatMulInputChunkHandler : public ChunkHandler {
public:
    MatMulInputChunkHandler(int16_t *_buffer_a) : buffer_a(_buffer_a) {}

    void operator () (uint32_t offset, uint16_t real_chunk_len, uint8_t state_bit) const override {
        if (state_bit) {
            int16_t* to_offset = buffer_a + offset;
            my_offset_q15(to_offset, -0x4000, to_offset, real_chunk_len);
        }
    }

private:
    int16_t *buffer_a;
};

class MatMulOutputChunkHandler : public ChunkHandler {
public:
    MatMulOutputChunkHandler(int16_t *_buffer_matmul) : buffer_matmul(_buffer_matmul) {}

    void operator () (uint32_t offset, uint16_t real_chunk_len, uint8_t state_bit) const override {
        if (!state_bit) {
            int16_t* to_offset = buffer_matmul + offset;
            my_offset_q15(to_offset, 0x4000, to_offset, real_chunk_len);
        }
    }

private:
    int16_t *buffer_matmul;
};

void handle_matmul(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    ParameterInfo *A = input[0], *B = input[1];

    my_printf_debug("handle_matmul inputs" NEWLINE);
    // dump_params_debug(model, A);
    my_printf_debug("B" NEWLINE);
    dump_params_debug(model, B);
    my_printf_debug("MatMul! A: (%dx%d), B: (%dx%d)" NEWLINE,
              A->dims[0], A->dims[1], B->dims[0], B->dims[1]);

    MY_ASSERT(A->dims[0] * A->dims[1] <= 256);

    int16_t A_len = A->dims[0] * A->dims[1];

    int16_t *buffer_a = lea_buffer,
            *buffer_temp = buffer_a + A_len,
            *buffer_matmul = buffer_temp + A->dims[0] * B->dims[1],
            *buffer_b = buffer_matmul + A->dims[0] * B->dims[1];

    my_fill_q15(0, buffer_matmul, 256);

    my_memcpy_from_param(buffer_a, A, 0, A->dims[0] * A->dims[1] * sizeof(uint16_t));

#if STATEFUL_CNN
    iterate_chunks(model, A, 0, 0, MatMulInputChunkHandler(buffer_a));
#endif

    /* LEA wants addresses to be 4-aligned */
    uint16_t step = (uint16_t)((256 / B->dims[1]) / 4 * 4);
    for (uint16_t i = 0; i < B->dims[0]; i = (uint16_t)(i + step)) {
        uint16_t current_width = (uint16_t)MIN_VAL(step, B->dims[0] - i);

        my_memcpy_from_param(buffer_b,
                  B, i * B->dims[1],
                  current_width * B->dims[1] * sizeof(uint16_t));

        my_printf_debug("strip for A" NEWLINE);
        dump_matrix_debug(buffer_a + A->dims[0] * i, (size_t)(A->dims[0] * current_width), ValueInfo(A, model));
        my_printf_debug("B" NEWLINE);
        dump_matrix_debug(buffer_b, (size_t)(current_width * B->dims[1]), ValueInfo(B, model));

        my_matrix_mpy_q15(A->dims[0], current_width, current_width, B->dims[1], buffer_a + A->dims[0] * i, buffer_b, buffer_temp, 0);

        my_printf_debug("temp" NEWLINE);
        dump_matrix_debug(buffer_temp, (size_t)(A->dims[0] * B->dims[1]), ValueInfo(output, model));

        my_add_q15(buffer_matmul, buffer_temp, buffer_matmul, output->params_len / sizeof(int16_t));
    }

#if STATEFUL_CNN
    iterate_chunks(model, output, 0, 0, MatMulOutputChunkHandler(buffer_matmul));
#endif

    my_memcpy_to_param(output, 0, buffer_matmul, output->params_len);

    my_printf_debug("handle_matmul output" NEWLINE);
    dump_params_debug(model, output);

#if STATEFUL_CNN
    flip_state_bit(model, output);
#endif
}

void alloc_relu(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    ParameterInfo *data = input[0];
    output->slot = get_next_slot(model, data);
    output->flags &= ~TRANSPOSED;
}

void handle_relu(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    my_printf_debug("ReLu!" NEWLINE);

    ParameterInfo *X = input[0];
    my_printf_debug("handle_relu input" NEWLINE);
    dump_params_nhwc_debug(model, X, 0);

    uint16_t CHANNEL = X->dims[1];

    /* XXX: use LEA? */
    uint16_t bitwidth = X->bitwidth;
    MY_ASSERT(bitwidth == 16);
    int16_t data_len = X->params_len / (bitwidth / 8);

#if STATEFUL_CNN
#endif

    uint16_t data_offset = 0;
    uint16_t output_offset = 0;
#if STATEFUL_CNN
    uint32_t first_unfinished_value_offset = recovery_from_state_bits(model, output);
    data_offset += first_unfinished_value_offset;
    output_offset += first_unfinished_value_offset;
#endif

    if (X->flags & TRANSPOSED) {
        // input is in NWHC
        // TODO: state-aware recovery
        uint16_t H = X->dims[2], W = X->dims[3];
        uint16_t output_h = 0, output_w = 0, c = 0;
#if STATEFUL_CNN
        output_h = first_unfinished_value_offset / (W * CHANNEL);
        first_unfinished_value_offset %= (W * CHANNEL);
        output_w = first_unfinished_value_offset / CHANNEL;
        c = first_unfinished_value_offset % CHANNEL;
        my_printf_debug("initial output_h = %d, ", output_h);
        my_printf_debug("initial output_w = %d, ", output_w);
        my_printf_debug("initial c = %d" NEWLINE, c);

        int16_t offset, next_output_turning_point;
        uint8_t output_turning_point_idx;
        SlotInfo *output_slot_info;
        find_initial_state_bit(&offset, &output_turning_point_idx, &next_output_turning_point, &output_slot_info, first_unfinished_value_offset, model, output);
        offset ^= 0x4000;
#endif
        for (; output_h < H; output_h++) {
            for (; output_w < W; output_w++) {
                for (; c < CHANNEL; c++) {
                    int16_t input_tile_c_index = c / X->tile_c;
                    int16_t input_tile_c_offset = c % X->tile_c;
                    uint16_t cur_input_tile_c = MIN_VAL(X->tile_c, CHANNEL - input_tile_c_index * X->tile_c);
                    int16_t val_offset = input_tile_c_index * W * H * X->tile_c + output_w * H * cur_input_tile_c + output_h * cur_input_tile_c + input_tile_c_offset;
                    int16_t input_val = get_q15_param(X, val_offset);
                    output_offset = output_h * W * CHANNEL + output_w * CHANNEL + c;
#if STATEFUL_CNN
                    // assuming input state bits are correct...
                    if (get_value_state_bit(input_val)) {
                        input_val -= 0x4000;
                    }
                    check_next_turning_point(offset, output_turning_point_idx, next_output_turning_point, output_slot_info, output_offset);
#endif
                    int16_t output_val = MAX_VAL(input_val, 0);
#if STATEFUL_CNN
                    output_val += offset;
#endif
                    put_q15_param(output, output_offset, output_val);
#if STATEFUL_CNN
                    my_printf_debug(
                        "output_h=% 3d, output_w=% 3d, c=% 3d, val_offset=% 6d, offset=% 6d, input val=% 6d, output_offset=% 6d, output val=% 6d" NEWLINE,
                        output_h, output_w, c, val_offset, offset, input_val, output_offset, output_val);
#else
                    my_printf_debug(
                        "output_h=% 3d, output_w=% 3d, c=% 3d, val_offset=% 6d, input val=% 6d, output_offset=% 6d, output val=% 6d" NEWLINE,
                        output_h, output_w, c, val_offset, input_val, output_offset, output_val);
#endif
                }
                c = 0;
            }
            output_w = 0;
        }
    } else {
        uint16_t i = 0;
#if STATEFUL_CNN
        ERROR_OCCURRED(); // TODO: adapt to range-based state assignments
#endif
        for (; i < data_len; i++) {
            put_q15_param(output, output_offset, MAX_VAL(get_q15_param(X, data_offset), 0));
            data_offset++;
            output_offset++;
        }
    }

    output->tile_c = CHANNEL;

#if STATEFUL_CNN
    flip_state_bit(model, output);
#endif

    my_printf_debug("handle_relu output" NEWLINE);
    dump_params_nhwc_debug(model, output, 0);
}

void handle_reshape(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    my_printf_debug("Reshape!" NEWLINE);

    ParameterInfo *data = input[0], *shape = input[1];
    output->params_offset = data->params_offset;
    output->params_len = data->params_len;
    output->bitwidth = data->bitwidth;
    output->slot = data->slot;
    SlotInfo *cur_slot_info = get_slot_info(model, output->slot);
    if (cur_slot_info) {
        cur_slot_info->user = model->layer_idx;
    }
    MY_ASSERT(shape->bitwidth == 64);
    /*
     * At most one dimension of the new shape can be -1. In this case, the
     * value is inferred from the size of the tensor and the remaining
     * dimensions.
     *
     * A dimension could also be 0, in which case the actual dimension value
     * is unchanged (i.e. taken from the input tensor).
     * */
    uint32_t new_len = 1;
    for (uint8_t i = 0; i < 4 && i < shape->dims[0]; i++) {
        output->dims[i] = (uint16_t)get_int64_param(shape, i);
        if (!output->dims[i]) {
            output->dims[i] = data->dims[i];
        }
        if (output->dims[i] != RESHAPE_AUTO_DIM) {
            new_len *= output->dims[i];
        }
    }
    for (uint8_t i = shape->dims[0]; i < 4; i++) {
        output->dims[i] = 0;
    }
    uint16_t inferred_dim = output->params_len / sizeof(int16_t);
    int8_t auto_idx = -1;
    for (uint8_t i = 0; i < 4; i++) {
        if (output->dims[i] != RESHAPE_AUTO_DIM && output->dims[i] != 0) {
            inferred_dim /= output->dims[i];
        } else if (output->dims[i] == RESHAPE_AUTO_DIM) {
            auto_idx = i;
        }
    }
    if (auto_idx != -1) {
        output->dims[auto_idx] = inferred_dim;
        new_len *= inferred_dim;
    }
    MY_ASSERT(new_len * sizeof(int16_t) == output->params_len);
}

void handle_squeeze(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    my_printf_debug("Squeeze!" NEWLINE);

    ParameterInfo *data = input[0];
    /* XXX: add flags; assume squeeze all one-size axes */
    output->params_offset = data->params_offset;
    output->params_len = data->params_len;
    output->bitwidth = data->bitwidth;
    output->slot = data->slot;
    SlotInfo *cur_slot_info = get_slot_info(model, output->slot);
    if (cur_slot_info) {
        cur_slot_info->user = model->layer_idx;
    }
    for (uint8_t i = 0, j = 0; i < 4; i++) {
        if (input[0]->dims[i] != 1) {
            output->dims[j] = input[0]->dims[i];
            j++;
        }
    }
}

void alloc_concat(Model *, ParameterInfo *[], ParameterInfo*, NodeFlags*) {
}

class ConcatOutputChunkHandler : public ChunkHandler {
public:
    ConcatOutputChunkHandler(uint32_t _offset) : offset(_offset) {}

    void operator () (uint32_t output_offset, uint16_t output_chunk_len, uint8_t old_output_state_bit) const override {
        my_printf_debug("output output_offset=%d output_chunk_len=%d old_output_state_bit=%d" NEWLINE, output_offset, output_chunk_len, old_output_state_bit);
        // every output chunk has the same starting offset as corresponding scaled input chunk
        int16_t *output_to_offset = lea_buffer + output_offset - offset;
        if (!old_output_state_bit) {
            my_offset_q15(output_to_offset, 0x4000, output_to_offset, output_chunk_len);
        }
    }

private:
    uint32_t offset;
};

class ConcatInputChunkHandler : public ChunkHandler {
public:
    ConcatInputChunkHandler(Model *_model, ParameterInfo *_scaled, float _scale, ParameterInfo *_param_in_new_slot)
        : model(_model), scaled(_scaled), scale(_scale), param_in_new_slot(_param_in_new_slot) {}

    void operator () (uint32_t offset, uint16_t real_chunk_len, uint8_t state_bit) const override {
        my_printf_debug("scaled range_offset=%d range_len=%d state_bit=%d" NEWLINE, offset, real_chunk_len, state_bit);
        my_memcpy_from_param(lea_buffer, scaled, offset, real_chunk_len * sizeof(int16_t));
#if STATEFUL_CNN
        if (state_bit) {
            my_offset_q15(lea_buffer, -0x4000, lea_buffer, real_chunk_len);
        }
#endif
        my_scale_q15(lea_buffer, scale * 32768, 0, lea_buffer, real_chunk_len);
#if STATEFUL_CNN
        iterate_chunks(model, param_in_new_slot, offset, real_chunk_len, ConcatOutputChunkHandler(offset));
#endif
        my_memcpy_to_param(param_in_new_slot, offset, lea_buffer, real_chunk_len * sizeof(int16_t));
    }

private:
    Model *model;
    ParameterInfo *scaled;
    float scale;
    ParameterInfo *param_in_new_slot;
};

void handle_concat(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    my_printf_debug("Concat!" NEWLINE);

    ParameterInfo *A = input[0], *B = input[1];
    // XXX: assume concatenating 2 tensors at the CHANNEL dimension and they
    // have the same number of channels.
    MY_ASSERT(A->dims[1] == B->dims[1]);
    output->tile_c = A->dims[1];
    output->dims[1] *= 2;
    output->flags |= SEPARATE_TILING;

    float scale;
    ParameterInfo *scaled = NULL;
    // The one with smaller `scale` (with larger values) is scaled down
    if (A->scale < B->scale) {
        scale = 1.0f * A->scale / B->scale;
        scaled = A;
        output->scale = A->scale = B->scale;
    } else if (A->scale > B->scale) {
        scale = 1.0f * B->scale / A->scale;
        scaled = B;
        output->scale = B->scale = A->scale;
    }
    if (scaled) {
        uint8_t new_slot = get_next_slot(model, scaled);
        ParameterInfo param_in_new_slot;
        my_memcpy(&param_in_new_slot, scaled, sizeof(struct ParameterInfo));
        param_in_new_slot.slot = new_slot;

        iterate_chunks(model, scaled, 0, 0, ConcatInputChunkHandler(model, scaled, scale, &param_in_new_slot));

        // XXX: touching nodes is dirty :(
        Node *nodes = (Node*)(model + 1);
        nodes[get_slot_info(model, output->slot)->user].max_output_id |= MAX_OUTPUT_ID_INVALID; // no longer used
        scaled->slot = new_slot;
#if STATEFUL_CNN
        flip_state_bit(model, scaled);
#endif
    }

    // saving slots here as it might be changed during the downscaling loop above
    output->extra_info[0] = A->parameter_info_idx;
    output->extra_info[1] = B->parameter_info_idx;
    output->slot = A->slot;

    dump_params_nhwc_debug(model, A, 0);
    dump_params_nhwc_debug(model, B, 0);
}

void handle_dropout(Model*, ParameterInfo*[], ParameterInfo*, NodeFlags*) {
    ERROR_OCCURRED();
}

void alloc_globalaveragepool(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    ParameterInfo *data = input[0];

    MY_ASSERT(data->dims[0] == 1);
    uint16_t output_len = data->dims[1];

    output->dims[0] = output->dims[2] = output->dims[3] = 1;
    output->dims[1] = data->dims[1];
    output->params_len = output_len * sizeof(int16_t);
    output->bitwidth = 16;
    output->slot = get_next_slot(model, data);
}

void handle_globalaveragepool(Model *model, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    my_printf_debug("GlobalAveragePool!" NEWLINE);

    ParameterInfo *data = input[0];

#if STATEFUL_CNN
    int16_t offset, next_output_turning_point;
    uint8_t output_turning_point_idx;
    SlotInfo *output_slot_info;
    find_initial_state_bit(&offset, &output_turning_point_idx, &next_output_turning_point, &output_slot_info, 0 /*TODO: first_unfinished_value_offset*/, model, output);
    offset ^= 0x4000;
#endif

    uint16_t CHANNEL = data->dims[1], H = data->dims[2], W = data->dims[3];
    uint16_t len = H * W;
    for (uint16_t c = 0; c < CHANNEL; c++) {
        uint32_t total = 0;
        for (uint16_t h = 0; h < H; h++) {
            for (uint16_t w = 0; w < W; w++) {
                // Input is from Conv, which uses NHWC
                int16_t val = get_q15_param(data, h * W * CHANNEL + w * CHANNEL + c);
#if STATEFUL_CNN
                if (get_value_state_bit(val)) {
                    val -= 0x4000;
                }
#endif
                total += val;
            }
        }
        int16_t avg = total / len;
#if STATEFUL_CNN
        check_next_turning_point(offset, output_turning_point_idx, next_output_turning_point, output_slot_info, c);
        avg += offset;
#endif
        put_q15_param(output, c, avg);
    }

#if STATEFUL_CNN
    flip_state_bit(model, output);
#endif

    dump_params_debug(model, output);
}

void handle_softmax(Model*, ParameterInfo*[], ParameterInfo*, NodeFlags*) {
    // Do nothing - softmax does not change the relative order of values.
    // Just let run_model determine the max value
}

void handle_transpose(Model*, ParameterInfo *input[], ParameterInfo *output, NodeFlags*) {
    my_printf_debug("Transpose!" NEWLINE);

    ParameterInfo *X = input[0];
    // not actually transpose data as we happen to need NHWC
    // XXX: assume NHWC -> NCHW
    output->dims[1] = X->dims[3];
    output->dims[2] = X->dims[1];
    output->dims[3] = X->dims[2];
}

class OverflowChunkHandler : public ChunkHandler {
public:
    OverflowChunkHandler(Model *_model, ParameterInfo *_param, uint16_t *_overflow_factor)
        : model(_model), param(_param), overflow_factor(_overflow_factor) {}

    void operator () (uint32_t offset, uint16_t real_chunk_len, uint8_t state_bit) const override {
#if !STATEFUL_CNN
        int16_t bound = 32768 / SCALE;
#else
        int16_t bound = 8192 / SCALE;
        int16_t val_offset = param_state_bit(model, param, offset) ? -16384 : 0;
#endif
        int16_t max_val, min_val;
        uint16_t index;

        my_memcpy_from_param(lea_buffer, param, offset, real_chunk_len * sizeof(int16_t));

        // dump_matrix(lea_buffer, real_chunk_len, ValueInfo(param));

        my_max_q15(lea_buffer, real_chunk_len, &max_val, &index);
#if STATEFUL_CNN
        max_val += val_offset;
#endif
        my_printf_debug("Max value %d", max_val);
        my_printf_debug(" occurs at index %d" NEWLINE, index);
        while (max_val && abs(max_val) >= bound * (*overflow_factor)) {
            (*overflow_factor) *= 2;
        }

        my_min_q15(lea_buffer, real_chunk_len, &min_val, &index);
#if STATEFUL_CNN
        min_val += val_offset;
#endif
        my_printf_debug("Min value %d", min_val);
        my_printf_debug(" occurs at index %d" NEWLINE, index);
        while (min_val && abs(min_val) >= bound * (*overflow_factor)) {
            (*overflow_factor) *= 2;
        }
    }
private:
    Model *model;
    ParameterInfo *param;
    uint16_t *overflow_factor;
};

uint16_t find_overflow_factor(Model *model, ParameterInfo *param) {
    uint16_t overflow_factor = 1;

    iterate_chunks(model, param, 0, 0, OverflowChunkHandler(model, param, &overflow_factor));

    my_printf_debug("Overflow factor = %d" NEWLINE, overflow_factor);

    return overflow_factor;
}

void float_to_scale_params(int16_t *scaleFract, uint8_t *shift, float scale) {
    *shift = 0;
    while (scale >= 1) {
        scale /= 2;
        (*shift)++;
    }
    *scaleFract = scale * 32768;
}

void iterate_chunks(Model *model, ParameterInfo *param, uint16_t start_offset, uint16_t len, const ChunkHandler& callback) {
    uint16_t params_len;
    if (!len) {
        params_len = param->params_len / sizeof(int16_t);
    } else {
        params_len = start_offset + len;
    }
    uint16_t chunk_len = LIMIT_DMA_SIZE((LEA_BUFFER_SIZE - 1) / 2 * 2);
    uint8_t state_bit = 0;

    uint16_t cur_chunk_len;
#if STATEFUL_CNN
    dump_turning_points_debug(model, param);

    state_bit = get_state_bit(model, param->slot);
    uint8_t turning_point_idx = 0;
    int16_t next_turning_point = -1;
    SlotInfo *cur_slot_info = get_slot_info(model, param->slot);
    uint16_t n_turning_points = cur_slot_info ? cur_slot_info->n_turning_points : 0;
    uint8_t turning_point_found = 0;
    while (turning_point_idx < n_turning_points) {
        next_turning_point = cur_slot_info->turning_points[turning_point_idx];
        turning_point_idx++;
        if (next_turning_point > start_offset) {
            turning_point_found = 1;
            break;
        }
        state_bit ^= 1;
    }
    if (!turning_point_found) {
        // no turning points not after start_offset found
        next_turning_point = -1;
    }
#endif
    for (uint32_t offset = start_offset; offset < params_len; offset += cur_chunk_len) {
        cur_chunk_len = MIN_VAL(chunk_len, params_len - offset);
#if STATEFUL_CNN
        uint8_t next_state_flipped = 0;
        // Use <= here as turning_point_idx is actually index for the _next_ turning point
        if (next_turning_point > 0 && turning_point_idx <= cur_slot_info->n_turning_points) {
            uint16_t chunk_len_before_turning_point = MIN_VAL(cur_chunk_len, next_turning_point - offset);
            if (chunk_len_before_turning_point != cur_chunk_len) {
                next_turning_point = cur_slot_info->turning_points[turning_point_idx];
                turning_point_idx++;
                next_state_flipped = 1;
            }
            cur_chunk_len = chunk_len_before_turning_point;
        }
#endif
        MY_ASSERT(cur_chunk_len != 0);
        callback(offset, cur_chunk_len, state_bit);
#if STATEFUL_CNN
        if (next_state_flipped) {
            state_bit ^= 1;
        }
#endif
    }
}
