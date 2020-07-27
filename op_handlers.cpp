#include <DSPLib.h>
#include <functional>

#include "cnn_common.h"
#include "op_handlers.h"
#include "debug.h"
#include "platform.h"
#include "conv.h"
#include "intermittent-cnn.h"

// Not using DSPLIB_DATA here as it does not work under C++ (?)
#ifdef __MSP430__
#pragma DATA_SECTION(".leaRAM")
#endif
int16_t lea_buffer[LEA_BUFFER_SIZE];

#define RESHAPE_AUTO_DIM (uint16_t)(-1)

void alloc_maxpool(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    uint16_t stride = flags & 0x0f;

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

static int16_t maxpool_patch(uint16_t output_h, uint16_t output_w, uint16_t c, uint16_t tile_c_offset, uint16_t flags, ParameterInfo *data, ParameterInfo *output, Model *model) {
    const uint16_t CHANNEL = data->dims[1], W = data->dims[3];
    uint16_t stride = flags & 0x0f;
    uint16_t kernel_size = (flags & 0xf0) >> 4;
    int16_t *data_baseptr = get_q15_param(data, 0);
#ifdef WITH_PROGRESS_EMBEDDING
    uint8_t input_state_bit = get_state_bit(model, data->slot);
    uint8_t old_output_state_bit = get_state_bit(model, output->slot);
#else
    UNUSED(output);
    UNUSED(model);
#endif

    int16_t offset_h, offset_w;
    offset_h = W * CHANNEL;
    offset_w = CHANNEL;

    my_printf_debug("output_h=%d ", output_h);
    my_printf_debug("output_w=%d ", output_w);
    my_printf_debug("c=%d" NEWLINE, tile_c_offset + c);

    int16_t max_val = INT16_MIN;
    for (uint16_t sH = 0; sH < kernel_size; sH++) {
        for (uint16_t sW = 0; sW < kernel_size; sW++) {
            int16_t val;
            // XXX: use a moving pointer instead of data_baseptr makes it slower. Why!?
            val = data_baseptr[(output_h*stride+sH) * offset_h + (output_w*stride+sW) * offset_w + tile_c_offset + c];
#ifdef WITH_PROGRESS_EMBEDDING
            if (input_state_bit) {
                val -= 0x4000;
            }
#endif
            print_q15_debug(val, data->scale, get_state_bit(model, data->slot));
            // XXX: use LEA?
            if (val > max_val) {
                max_val = val;
            }
        }
    }
#ifdef WITH_PROGRESS_EMBEDDING
    if (!old_output_state_bit) {
        max_val += 0x4000;
    }
#endif
    // need a space as print_q15_debug does not append spaces when DUMP_INTEGERS is not defined
    my_printf_debug(" max=");
    print_q15_debug(max_val, data->scale, get_state_bit(model, data->slot));
    return max_val;
}

void handle_maxpool(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {

    my_printf_debug("MaxPool!" NEWLINE);

    uint16_t stride = flags & 0x0f;
    uint8_t need_nhwc2nchw = ((flags & 0xff00) >> 8 == NHWC2NCHW);

    /* XXX: add flags; assume no padding for now */
    ParameterInfo *data = input[0];

    my_printf_debug("handle_maxpool input" NEWLINE);
    dump_params(model, data);

    const uint16_t CHANNEL = data->dims[1], H = data->dims[2], W = data->dims[3];
    uint16_t new_H = H / stride;
    uint16_t new_W = W / stride;

    uint16_t tile_c = get_tile_c(output);
    my_printf_debug("tile_c = %d" NEWLINE, tile_c);

    uint32_t first_unfinished_value_offset = recovery_from_state_bits(model, output);
    uint16_t initial_n, initial_c, initial_h, initial_w;
    initial_n = first_unfinished_value_offset / (new_H * new_W * tile_c);

    my_printf_debug("initial_n = %d" NEWLINE, initial_n);

    int16_t *output_baseptr = get_q15_param(output, 0);
    for (uint16_t tile_c_offset = initial_n * tile_c; tile_c_offset < CHANNEL; tile_c_offset += tile_c) {
        uint16_t real_tile_c = MIN_VAL(tile_c, CHANNEL - tile_c_offset);
        int16_t *output_ptr = output_baseptr + tile_c_offset * new_H * new_W;
        if (!need_nhwc2nchw) {
            // NHWC
            initial_c = first_unfinished_value_offset % real_tile_c;
            first_unfinished_value_offset /= real_tile_c;
            initial_w = first_unfinished_value_offset % new_W;
            first_unfinished_value_offset /= new_W;
            initial_h = first_unfinished_value_offset % new_H;

            my_printf_debug("initial_h = %d" NEWLINE, initial_h);
            my_printf_debug("initial_w = %d" NEWLINE, initial_w);
            my_printf_debug("initial_c = %d" NEWLINE, initial_c);

            uint16_t output_h = 0;
            if (tile_c_offset == initial_n * tile_c) {
                output_h = initial_h;
                output_ptr += initial_h * new_W * real_tile_c;
            }
            for (; output_h < new_H; output_h++) {
                uint16_t output_w = 0;
                if (tile_c_offset == initial_n * tile_c && output_h == initial_h) {
                    output_w = initial_w;
                    output_ptr += initial_w * real_tile_c;
                }
                for (; output_w < new_W; output_w++) {
                    uint16_t c = 0;
                    if (tile_c_offset == initial_n * tile_c && output_h == initial_h && output_w == initial_w) {
                        c = initial_c;
                        output_ptr += initial_c;
                    }
                    for (; c < real_tile_c; c++) {
                        int16_t max_val = maxpool_patch(output_h, output_w, c, tile_c_offset, flags, data, output, model);
                        my_printf_debug(NEWLINE "offset=%d" NEWLINE, (uint16_t)(output_ptr - output_baseptr));
                        *output_ptr = max_val;
                        output_ptr++;
                    }
                }
            }
        } else {
            // NCHW
            initial_w = first_unfinished_value_offset % new_W;
            first_unfinished_value_offset /= new_W;
            initial_h = first_unfinished_value_offset % new_H;
            first_unfinished_value_offset /= new_H;
            initial_c = first_unfinished_value_offset % real_tile_c;

            my_printf_debug("initial_h = %d" NEWLINE, initial_h);
            my_printf_debug("initial_w = %d" NEWLINE, initial_w);
            my_printf_debug("initial_c = %d" NEWLINE, initial_c);

            uint16_t c = 0;
            if (tile_c_offset == initial_n * tile_c) {
                c = initial_c;
                output_ptr += initial_c * new_H * new_W;
            }
            for (; c < real_tile_c; c++) {
                uint16_t output_h = 0;
                if (tile_c_offset == initial_n * tile_c && c == initial_c) {
                    output_h = initial_h;
                    output_ptr += initial_h * new_W;
                }
                for (; output_h < new_H; output_h++) {
                    uint16_t output_w = 0;
                    if (tile_c_offset == initial_n * tile_c && c == initial_c && output_h == initial_h) {
                        output_w = initial_w;
                        output_ptr += initial_w;
                    }
                    for (; output_w < new_W; output_w++) {
                        int16_t max_val = maxpool_patch(output_h, output_w, c, tile_c_offset, flags, data, output, model);
                        my_printf_debug(NEWLINE "offset=%d" NEWLINE, (uint16_t)(output_ptr - output_baseptr));
                        *output_ptr = max_val;
                        output_ptr++;
                    }
                }
            }
        }
    }

    flip_state_bit(model, output);

    my_printf_debug("handle_maxpool output" NEWLINE);
    if (!need_nhwc2nchw) {
        for (uint16_t c = 0; c < CHANNEL; c += tile_c) {
            output->dims[1] = MIN_VAL(tile_c, CHANNEL - c);
            dump_params_nhwc(model, output, c * new_H * new_W);
        }
        output->dims[1] = CHANNEL;
    } else if (tile_c == CHANNEL) {
        dump_params(model, output);
    }
}

void alloc_add(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(flags);

    ParameterInfo *A = input[0], *B = input[1];
    MY_ASSERT(A->bitwidth == 16 && B->bitwidth == 16);

    output->slot = get_next_slot(model, A);
}

void handle_add(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(flags);

    /* Add: Y = X + W */
    my_printf_debug("Add!" NEWLINE);

    ParameterInfo *A = input[0], *B = input[1];

    msp_add_q15_params add_params;
    add_params.length = A->dims[1];

    int16_t *buffer_a = lea_buffer,
            *buffer_b = lea_buffer + output->params_len / sizeof(int16_t);
    my_memcpy(buffer_a, get_q15_param(A, 0), output->params_len);
    my_memcpy(buffer_b, get_q15_param(B, 0), output->params_len);
    msp_status status;
    msp_scale_q15_params scale_params;
    scale_params.length = add_params.length;
    if (A->scale > B->scale) {
        float_to_scale_params(&scale_params, 1.0f * B->scale / A->scale);
        status = msp_scale_q15(&scale_params, buffer_b, buffer_b);
        msp_checkStatus(status);
    } else if (B->scale > A->scale) {
        float_to_scale_params(&scale_params, 1.0f * A->scale / B->scale);
        status = msp_scale_q15(&scale_params, buffer_a, buffer_a);
        msp_checkStatus(status);
    }
    status = msp_add_q15(&add_params, buffer_a, buffer_b, buffer_a);
    msp_checkStatus(status);

    my_memcpy(get_q15_param(output, 0), buffer_a, output->params_len);
}

void alloc_matmul(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(flags);

    ParameterInfo *A = input[0], *B = input[1];

    uint16_t output_len = A->dims[0] * B->dims[1];

    output->dims[0] = A->dims[0];
    output->dims[1] = B->dims[1];
    output->params_len = output_len * sizeof(int16_t);
    output->bitwidth = 16;
    output->slot = get_next_slot(model, A);
    output->scale = A->scale * B->scale;
}

void handle_matmul(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(flags);

    ParameterInfo *A = input[0], *B = input[1];

    my_printf_debug("handle_matmul inputs" NEWLINE);
    // dump_params(model, A);
    my_printf_debug("B" NEWLINE);
    dump_params(model, B);
    my_printf_debug("MatMul! A: (%dx%d), B: (%dx%d)" NEWLINE,
              A->dims[0], A->dims[1], B->dims[0], B->dims[1]);

    MY_ASSERT(A->dims[0] * A->dims[1] <= 256);

    int16_t A_len = A->dims[0] * A->dims[1];

    int16_t *buffer_a = lea_buffer,
            *buffer_temp = buffer_a + A_len,
            *buffer_matmul = buffer_temp + A->dims[0] * B->dims[1],
            *buffer_b = buffer_matmul + A->dims[0] * B->dims[1];

    msp_fill_q15_params fill_params;
    fill_params.length = 256;
    fill_params.value = 0;
    msp_status status = msp_fill_q15(&fill_params, buffer_matmul);
    msp_checkStatus(status);

    my_memcpy(buffer_a, get_q15_param(A, 0), A->dims[0] * A->dims[1] * sizeof(uint16_t));

#ifdef WITH_PROGRESS_EMBEDDING
    if (get_state_bit(model, A->slot)) {
        for (uint16_t idx = 0; idx < A_len; idx++) {
            buffer_a[idx] -= 0x4000;
        }
    }
#endif

    /* LEA wants addresses to be 4-aligned */
    uint16_t step = (uint16_t)((256 / B->dims[1]) / 4 * 4);
    for (uint16_t i = 0; i < B->dims[0]; i = (uint16_t)(i + step)) {
        msp_matrix_mpy_q15_params params;
        uint16_t current_width = (uint16_t)MIN_VAL(step, B->dims[0] - i);
        params.srcARows = A->dims[0];
        params.srcACols = current_width;
        params.srcBRows = current_width;
        params.srcBCols = B->dims[1];

        my_memcpy(buffer_b,
                  get_q15_param(B, i * B->dims[1]),
                  current_width * B->dims[1] * sizeof(uint16_t));

        my_printf_debug("strip for A" NEWLINE);
        dump_matrix(buffer_a + A->dims[0] * i, (size_t)(A->dims[0] * current_width), A->scale, get_state_bit(model, A->slot));
        my_printf_debug("B" NEWLINE);
        dump_matrix(buffer_b, (size_t)(current_width * B->dims[1]), B->scale, get_state_bit(model, B->slot));

        status = msp_matrix_mpy_q15(
            &params,
            buffer_a + A->dims[0] * i,
            buffer_b,
            buffer_temp);
        msp_checkStatus(status);

        my_printf_debug("temp" NEWLINE);
        dump_matrix(buffer_temp, (size_t)(A->dims[0] * B->dims[1]), output->scale, get_state_bit(model, output->slot));

        msp_add_q15_params params2;
        params2.length = output->params_len / sizeof(int16_t);
        status = msp_add_q15(&params2, buffer_matmul, buffer_temp, buffer_matmul);
        msp_checkStatus(status);
    }
    my_memcpy(get_q15_param(output, 0), buffer_matmul, output->params_len);

    my_printf_debug("handle_matmul output" NEWLINE);
    dump_params(model, output);

    flip_state_bit(model, output);
}

void alloc_relu(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(flags);

    ParameterInfo *data = input[0];
    output->slot = get_next_slot(model, data);
    output->flags &= ~TRANSPOSED;
}

void handle_relu(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(flags);

    my_printf_debug("ReLu!" NEWLINE);

    uint32_t first_unfinished_value_offset = recovery_from_state_bits(model, output);

    ParameterInfo *X = input[0];
    my_printf_debug("handle_relu input" NEWLINE);
    dump_params_nhwc(model, X, 0);

    /* XXX: use LEA? */
    uint16_t bitwidth = X->bitwidth;
    MY_ASSERT(bitwidth == 16);
    int16_t *data_baseptr = get_q15_param(X, 0);
    int16_t *output_baseptr = get_q15_param(output, 0);
    int16_t data_len = X->params_len / (bitwidth / 8);

    int16_t threshold = 0, offset = 0;
#ifdef WITH_PROGRESS_EMBEDDING
    if (get_state_bit(model, X->slot)) {
        threshold = 0x4000;
        offset = -0x4000;
    }
    if (!get_state_bit(model, output->slot)) {
        offset += 0x4000;
    }
#endif

    my_printf_debug("threshold = %d" NEWLINE, threshold);
    my_printf_debug("offset = %d" NEWLINE, offset);

    int16_t *data_ptr = data_baseptr + first_unfinished_value_offset;
    int16_t *output_ptr = output_baseptr + first_unfinished_value_offset;
    if (X->flags & TRANSPOSED) {
        // input is in NWHC
        // TODO: state-aware recovery
        uint16_t CHANNEL = X->dims[1], H = X->dims[2], W = X->dims[3];
        for (uint16_t output_h = 0; output_h < H; output_h++) {
            for (uint16_t output_w = 0; output_w < W; output_w++) {
                for (uint16_t c = 0; c < CHANNEL; c++) {
                    int16_t val = *(data_baseptr + output_w * H * CHANNEL + output_h * CHANNEL + c);
                    *(output_baseptr + output_h * W * CHANNEL + output_w * CHANNEL + c) = MAX_VAL(val, threshold) + offset;
                }
            }
        }
    } else {
        for (uint16_t i = first_unfinished_value_offset; i < data_len; i++) {
            *output_ptr = MAX_VAL(*data_ptr, threshold) + offset;
            data_ptr++;
            output_ptr++;
        }
    }

    flip_state_bit(model, output);

    my_printf_debug("handle_relu output" NEWLINE);
    dump_params_nhwc(model, output, 0);
}

void handle_reshape(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(flags);

    my_printf_debug("Reshape!" NEWLINE);

    ParameterInfo *data = input[0], *shape = input[1];
    output->params_offset = data->params_offset;
    output->params_len = data->params_len;
    output->bitwidth = data->bitwidth;
    output->slot = data->slot;
    model->slot_users[output->slot] = model->layer_idx;
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
    MY_ASSERT(new_len * sizeof(int16_t) == output->params_len)
}

void handle_squeeze(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(flags);

    my_printf_debug("Squeeze!" NEWLINE);

    ParameterInfo *data = input[0];
    /* XXX: add flags; assume squeeze all one-size axes */
    output->params_offset = data->params_offset;
    output->params_len = data->params_len;
    output->bitwidth = data->bitwidth;
    output->slot = data->slot;
    model->slot_users[output->slot] = model->layer_idx;
    for (uint8_t i = 0, j = 0; i < 4; i++) {
        if (input[0]->dims[i] != 1) {
            output->dims[j] = input[0]->dims[i];
            j++;
        }
    }
}

static void iterate_chunks(ParameterInfo *param, std::function<void(uint32_t, uint16_t)> callback) {
    uint16_t params_len = param->params_len / sizeof(int16_t);
    uint16_t chunk_len = (LEA_BUFFER_SIZE - 1) / 2 * 2;

    for (uint32_t offset = 0; offset < params_len; offset += chunk_len) {
        uint16_t real_chunk_len = MIN_VAL(chunk_len, params_len - offset);
        callback(offset, real_chunk_len);
    }
}

void alloc_concat(struct Model *model, struct ParameterInfo *input[], struct ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(input);
    UNUSED(output);
    UNUSED(flags);
}

void handle_concat(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(flags);

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
    int16_t *scaled_srcptr, *scaled_dstptr;
    // The one with smaller `scale` (with larger values) is scaled down
    if (A->scale < B->scale) {
        scale = 1.0f * A->scale / B->scale;
        scaled = A;
        scaled_srcptr = get_q15_param(A, 0);
        output->scale = A->scale = B->scale;
    } else if (A->scale > B->scale) {
        scale = 1.0f * B->scale / A->scale;
        scaled = B;
        scaled_srcptr = get_q15_param(B, 0);
        output->scale = B->scale = A->scale;
    }
    if (scaled) {
        msp_status status;
        uint8_t orig_slot = scaled->slot;
        uint8_t new_slot = get_next_slot(model, scaled);
        uint8_t old_output_state_bit = get_state_bit(model, new_slot);
        ParameterInfo tmp_param;
        my_memcpy(&tmp_param, scaled, sizeof(struct ParameterInfo));
        tmp_param.slot = new_slot;
        scaled_dstptr = get_q15_param(&tmp_param, 0);

        iterate_chunks(scaled, [&] (uint32_t offset, uint16_t real_chunk_len) {
            my_memcpy(lea_buffer, scaled_srcptr + offset, real_chunk_len * sizeof(int16_t));
#ifdef WITH_PROGRESS_EMBEDDING
            msp_offset_q15_params offset_params;
            offset_params.length = real_chunk_len;
            offset_params.offset = model->state_bit[orig_slot] ? -0x4000 : 0;
            status = msp_offset_q15(&offset_params, lea_buffer, lea_buffer);
            msp_checkStatus(status);
#endif
            msp_scale_q15_params scale_params;
            scale_params.length = real_chunk_len;
            scale_params.scale = _Q15(scale);
            scale_params.shift = 0;
            status = msp_scale_q15(&scale_params, lea_buffer, lea_buffer);
            msp_checkStatus(status);
#ifdef WITH_PROGRESS_EMBEDDING
            offset_params.offset = old_output_state_bit ? 0 : 0x4000;
            status = msp_offset_q15(&offset_params, lea_buffer, lea_buffer);
            msp_checkStatus(status);
#endif
            my_memcpy(scaled_dstptr + offset, lea_buffer, real_chunk_len * sizeof(int16_t));
        });

        // XXX: touching nodes is dirty :(
        Node *nodes = (Node*)(model + 1);
        nodes[model->slot_users[scaled->slot]].max_output_id |= MAX_OUTPUT_ID_INVALID; // no longer used
        scaled->slot = new_slot;
        flip_state_bit(model, scaled);
    }

    // saving slots here as it might be changed during the downscaling loop above
    output->extra_info[0] = output->slot = A->slot;
    output->extra_info[1] = B->slot;

    dump_params_nhwc(model, A, 0);
    dump_params_nhwc(model, B, 0);
}

void handle_dropout(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(input);
    UNUSED(output);
    UNUSED(flags);

    ERROR_OCCURRED();
}

void alloc_globalaveragepool(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(flags);

    ParameterInfo *data = input[0];

    MY_ASSERT(data->dims[0] == 1);
    uint16_t output_len = data->dims[1];

    output->dims[0] = output->dims[2] = output->dims[3] = 1;
    output->dims[1] = data->dims[1];
    output->params_len = output_len * sizeof(int16_t);
    output->bitwidth = 16;
    output->slot = get_next_slot(model, data);
}

void handle_globalaveragepool(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(output);
    UNUSED(flags);

    my_printf_debug("GlobalAveragePool!" NEWLINE);

    ParameterInfo *data = input[0];
    uint16_t CHANNEL = data->dims[1], H = data->dims[2], W = data->dims[3];
    int16_t *input_baseptr = get_q15_param(data, 0),
            *output_baseptr = get_q15_param(output, 0);
    uint16_t len = H * W;
    for (uint16_t c = 0; c < CHANNEL; c++) {
        uint32_t total = 0;
        for (uint16_t h = 0; h < H; h++) {
            for (uint16_t w = 0; w < W; w++) {
                // Input is from Conv, which uses NHWC
                total += input_baseptr[h * W * CHANNEL + w * CHANNEL + c];
            }
        }
        output_baseptr[c] = total / len;
    }

    dump_params(model, output);
}

void handle_softmax(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(input);
    UNUSED(output);
    UNUSED(flags);

    // Do nothing - softmax does not change the relative order of values.
    // Just let run_model determine the max value
}

void handle_transpose(Model *model, ParameterInfo *input[], ParameterInfo *output, uint16_t flags) {
    UNUSED(model);
    UNUSED(flags);

    my_printf_debug("Transpose!" NEWLINE);

    ParameterInfo *X = input[0];
    // not actually transpose data as we happen to need NHWC
    // XXX: assume NHWC -> NCHW
    output->dims[1] = X->dims[3];
    output->dims[2] = X->dims[1];
    output->dims[3] = X->dims[2];
}

uint16_t find_overflow_factor(Model *model, ParameterInfo *param) {
    uint16_t overflow_factor = 1;
    int16_t max_val, min_val;
    uint16_t index;
#ifndef WITH_PROGRESS_EMBEDDING
    int16_t min_bound = -32768 / SCALE;
    int16_t max_bound = 32767 / SCALE;
    UNUSED(model);
#else
    int16_t min_bound = -8192 / SCALE;
    int16_t max_bound = 8191 / SCALE;
    int16_t val_offset = get_state_bit(model, param->slot) ? -16384 : 0;
#endif

    msp_status status;

    iterate_chunks(param, [&] (uint32_t offset, uint16_t real_chunk_len) {
        my_memcpy(lea_buffer, get_q15_param(param, offset), real_chunk_len * sizeof(int16_t));

        msp_max_q15_params max_params;
        max_params.length = real_chunk_len;
        status = msp_max_q15(&max_params, lea_buffer, &max_val, &index);
        msp_checkStatus(status);
#ifdef WITH_PROGRESS_EMBEDDING
        max_val += val_offset;
#endif
        my_printf_debug("Max value %d", max_val);
        my_printf_debug(" occurs at index %d" NEWLINE, index);
        while (max_val && max_val >= max_bound * overflow_factor) {
            overflow_factor *= 2;
        }

        msp_min_q15_params min_params;
        min_params.length = real_chunk_len;
        status = msp_min_q15(&min_params, lea_buffer, &min_val, &index);
        msp_checkStatus(status);
#ifdef WITH_PROGRESS_EMBEDDING
        min_val += val_offset;
#endif
        my_printf_debug("Min value %d", min_val);
        my_printf_debug(" occurs at index %d" NEWLINE, index);
        while (min_val && min_val <= min_bound * overflow_factor) {
            overflow_factor *= 2;
        }
    });

    my_printf_debug("Overflow factor = %d" NEWLINE, overflow_factor);

    return overflow_factor;
}

void float_to_scale_params(msp_scale_q15_params *scale_params, float scale) {
    scale_params->shift = 0;
    while (scale >= 1) {
        scale /= 2;
        scale_params->shift++;
    }
    scale_params->scale = _Q15(scale);
}
