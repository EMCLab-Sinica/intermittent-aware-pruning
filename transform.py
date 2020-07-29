#!/usr/bin/python
import argparse
import dataclasses
import io
import itertools
import pprint
import struct
import warnings
from typing import List

import onnx
import onnx.helper
import onnx.optimizer
import numpy as np

from utils import load_data, load_data_cifar10

"""
Goal: Mapping name-based nodes to integer-based ones.
Indexing policy:
    0~len(g.input)-1: input nodes
    len(g.input)~ : other (hidden) nodes
"""

class Constants:
    SLOT_PARAMETERS2 = 0xf1
    SLOT_PARAMETERS = 0xf0
    SLOT_TEST_SET = 0xff
    SLOT_INTERMEDIATE_VALUES = 0b01
    COUNTERS_LEN = 64
    # To make the Node struct exactly 64 bytes
    NODE_NAME_LEN = 54
    MAX_OUTPUT_ID_INVALID = 0x8000

# https://github.com/onnx/onnx/blob/master/docs/Operators.md
# [expected_inputs_len, inplace_update]
ops = {
    'Add': [2, 0],
    # Concat actually accepts 1~infinity inputs. Use 2 to fit SqueezeNet
    'Concat': [2, 0],
    'Conv': [3, 0],
    'ConvMerge': [1, 0],
    'Dropout': [1, 1],
    'GlobalAveragePool': [1, 0],
    'MatMul': [2, 0],
    'MaxPool': [1, 0],
    'Relu': [1, 0],
    'Reshape': [2, 1],
    'Softmax': [1, 1],
    'Squeeze': [1, 1],
    # XXX: Transpose does nothing as we happens to need NHWC
    'Transpose': [1, 1],
}

other_flags = [
    'AUTO_PAD_VALID',
    'NHWC2NCHW',
    'TRANSPOSED',
    # Tiles in different channels are actually in different slots
    'SEPARATE_TILING',
]

def op_flag(flag):
    return 2 ** other_flags.index(flag)

def _Q15(num):
    """Transform a floating point number to TI's fixed point _q15 format"""

    # See DSPLib_1_30_00_02/include/DSPLib_support.h

    lower = -1
    upper = 32767.0 / 32768.0

    if num < lower or num >= upper:
        if num != 1.0:
            warnings.warn(
                'Number {} goes beyond the range of _q15 ({}, {})'.format(
                    num, lower, upper))
        num = max(min(num, upper), lower)

    return int(num * 2 ** 15)


class ONNXNodeWrapper:
    def __init__(self, orig_node: onnx.NodeProto, flags: int = 0):
        self.orig_node = orig_node
        self.flags = flags

    def __getattr__(self, name):
        return getattr(self.orig_node, name)


def get_prev_node(n):
    return nodes[names[n.input[0]] - n_input]

# intermediate_values_size should < 65536, or TI's compiler gets confused
configs = {
    'mnist': {
        # https://github.com/onnx/models/raw/master/vision/classification/mnist/model/mnist-8.onnx
        'onnx_model': 'data/mnist-8.onnx',
        'input_file': 'data/Test-28x28_cntk_text.txt',
        'scale': 8,
        'num_slots': 2,
        'intermediate_values_size': 31000,
        'nvm_size': 256 * 1024,
        'data_loader': load_data,
        'n_samples': 20,
        'n_all_samples': 10000,
        'fp32_accuracy': 0.9889,
    },
    'cifar10': {
        'onnx_model': 'data/squeezenet_cifar10.onnx',
        'input_file': 'data/cifar10-test_batch',
        'scale': 8,
        'num_slots': 3,
        'intermediate_values_size': 30000,
        'nvm_size': 1024 * 1024,
        'data_loader': load_data_cifar10,
        'n_samples': 20,
        'n_all_samples': 10000,
        'fp32_accuracy': 0.7553,
    },
}

parser = argparse.ArgumentParser()
parser.add_argument('config', choices=configs.keys())
parser.add_argument('--without-progress-embedding', action='store_true')
parser.add_argument('--all-samples', action='store_true')
args = parser.parse_args()
config = configs[args.config]
if args.all_samples:
    config['nvm_size'] *= 64
    config['n_samples'] = config['n_all_samples']

original_model = onnx.load(config['onnx_model'])
try:
    # https://zhuanlan.zhihu.com/p/41255090
    onnx_model = onnx.optimizer.optimize(original_model, ['fuse_add_bias_into_conv'])
except IndexError:
    # Somehow the optimizer cannot handle models transformed from keras2onnx
    onnx_model = original_model
g = onnx_model.graph
names = {}

# Remoe Squeeze nodes with constants as the input
replaced_squeeze_map = {}
for n in g.node:
    if n.op_type != 'Squeeze':
        continue
    input_name = n.input[0]
    for inp in g.initializer:
        if input_name != inp.name:
            continue
        axes = next(attr.ints for attr in n.attribute if attr.name == 'axes')
        new_dims = [dim for dim_idx, dim in enumerate(inp.dims) if dim_idx not in axes]
        # Repeated fields cannot be assigned directly
        # https://developers.google.com/protocol-buffers/docs/reference/python-generated#repeated-fields
        inp.dims[:] = new_dims
        replaced_squeeze_map[n.output[0]] = input_name
        break

# Split Conv into Conv and ConvMerge (for OFM scaling up and merge of OFMs from  channel tiling)
new_nodes = []
for idx, n in enumerate(g.node):
    new_nodes.append(n)
    if n.op_type != 'Conv':
        continue
    output_name = n.output[0]
    new_node = onnx.NodeProto()
    new_node.name = n.name + ':merge'
    new_node.op_type = 'ConvMerge'
    new_node.input[:] = n.output[:] = [output_name + '_before_merge']
    new_node.output[:] = [output_name]
    new_nodes.append(new_node)

new_nodes = [n for n in new_nodes if n.output[0] not in replaced_squeeze_map.keys()]
for n in new_nodes:
    for idx, inp in enumerate(n.input):
        n.input[idx] = replaced_squeeze_map.get(inp, inp)

nodes = [ONNXNodeWrapper(n) for n in new_nodes]

conv_param_names = set()

for idx, inp in enumerate(g.input):
    names[inp.name] = idx

# For some ONNX models (e.g., squeezenet-cifar10 converted from Keras), inputs
# do not include initializers. Merge them here.
inputs_len = len(names.keys())
for idx, initializer in enumerate(g.initializer):
    if initializer.name not in names:
        names[initializer.name] = idx + inputs_len

n_input = len(names.keys())
print("n_input = {}".format(n_input))

def get_attr(node, attr_name):
    for attr in node.attribute:
        if attr.name != attr_name:
            continue
        return onnx.helper.get_attribute_value(attr)

    # Not found
    return None

prev_node = None
for idx, n in enumerate(nodes):
    if n.op_type == 'Dropout':
        output = n.output[:1]  # we don't care the second output `mask`
    else:
        output = n.output
    assert len(output) == 1
    if n.op_type == 'Conv':
        # https://github.com/onnx/onnx/blob/master/docs/Operators.md#conv
        conv_param_names.add(n.input[1])
        auto_pad = get_attr(n, 'auto_pad')
        if auto_pad == b'VALID':
            n.flags += op_flag('AUTO_PAD_VALID') * 0x100
    if n.op_type == 'MaxPool':
        kernel_shape = get_attr(n, 'kernel_shape')
        if kernel_shape is not None:
            n.flags += kernel_shape[0] * 0x10
    if n.op_type in ('MaxPool', 'Conv'):
        stride = get_attr(n, 'strides')[0]
        n.flags += stride
    if n.op_type == 'Reshape' and prev_node and prev_node.op_type == 'MaxPool':
        prev_node.flags += op_flag('NHWC2NCHW') * 0x100
    names[output[0]] = idx + n_input
    prev_node = n

pprint.pprint(names)

@dataclasses.dataclass
class Node:
    name: str
    inputs: List[int]
    op_type: str
    flags: int
    max_output_id: int

model = []
for n in nodes:
    model.append(Node(n.name, [names[i] for i in n.input], n.op_type, n.flags, 0))

for idx, node in enumerate(model):
    for inp in node.inputs:
        if inp < n_input:
            continue
        used_node = model[inp - n_input]
        used_node.max_output_id = max([idx, used_node.max_output_id])

# Inputs of Concat should be kept until Concat is processed
for idx, node in enumerate(model):
    if node.op_type != 'Concat':
        continue
    for inp in node.inputs:
        if inp < n_input:
            continue
        used_node = model[inp - n_input]
        used_node.max_output_id = max([used_node.max_output_id, node.max_output_id])

parameters = [None for _ in range(n_input)]

for params in g.initializer:
    if params.data_type not in (onnx.TensorProto.FLOAT, onnx.TensorProto.INT64):
        raise Exception('unsupported data type {}'.format(params.data_type))

    assert parameters[names[params.name]] is None
    parameters[names[params.name]] = params

pprint.pprint(model)

def to_bytes(i, size=16):
    if size == 8:
        return struct.pack('B', i)  # unsigned char
    elif size == 16:
        return struct.pack('h', i)
    elif size == 32:
        return struct.pack('i', i)
    elif size == 64:
        return struct.pack('q', i)
    else:
        raise ValueError(f'Unsupported size {size}')

def nchw2nhwc(arr, dims):
    N, C, H, W = dims
    ret = [0] * (N * C * H * W)
    for n in range(N):
        for c in range(C):
            for h in range(H):
                for w in range(W):
                    old_idx = n * C * H * W + c * H * W + h * W + w
                    new_idx = n * H * W * C + h * W * C + w * C + c
                    ret[new_idx] = arr[old_idx]
    return ret, (N, H, W, C)

inputs_data = io.BytesIO()
outputs = {
    'parameters': io.BytesIO(),
    'parameters2': io.BytesIO(),
    'samples': io.BytesIO(),
    'model': io.BytesIO(),
    'labels': io.BytesIO(),
    'counters': io.BytesIO(),
}

outputs['model'].write(to_bytes(len(model)))
outputs['model'].write(to_bytes(n_input))
outputs['model'].write(to_bytes(0))  # Model.running
outputs['model'].write(to_bytes(0))  # Model.recovery
outputs['model'].write(to_bytes(0))  # Model.run_counter
for _ in range(config['num_slots']):
    outputs['model'].write(to_bytes(0))  # Model.state_bit
for _ in range(config['num_slots']):
    outputs['model'].write(to_bytes(-1))  # Model.slot_users
outputs['model'].write(to_bytes(0))  # Model.layer_idx
outputs['model'].write(to_bytes(0))  # Model.sample_idx

@dataclasses.dataclass
class ParametersSlot:
    offset: int
    target: io.BytesIO
    slot_id: int

parameters_slot = ParametersSlot(offset=0, target=outputs['parameters'], slot_id=Constants.SLOT_PARAMETERS)
parameters2_slot = ParametersSlot(offset=0, target=outputs['parameters2'], slot_id=Constants.SLOT_PARAMETERS2)

for node in model:
    assert len(node.name) < Constants.NODE_NAME_LEN
    outputs['model'].write(node.name.encode('ascii') + b'\0' * (Constants.NODE_NAME_LEN - len(node.name)))
    outputs['model'].write(to_bytes(len(node.inputs)))
    outputs['model'].write(to_bytes(inputs_data.tell()))  # Node.inputs_offset
    outputs['model'].write(to_bytes(node.max_output_id))
    for inp in node.inputs:
        # the lowest bit is used as a flag in topological sort
        inputs_data.write(to_bytes(inp * 2))
    outputs['model'].write(to_bytes(list(ops.keys()).index(node.op_type)))
    outputs['model'].write(to_bytes(node.flags))


labels, images = config['data_loader'](config['input_file'], limit=config['n_samples'])

def select_parameters_slot(data_len):
    if data_len <= 1024:  # XXX: random heuristic
        return parameters_slot
    else:
        return parameters2_slot

for params in parameters:
    if params is None:  # input
        # Actual data for test samples are added last
        _, input_channel, dimX, dimY = images[0].shape
        outputs['model'].write(to_bytes(parameters_slot.offset, size=32))  # params_offset
        outputs['model'].write(to_bytes(input_channel* dimX * dimY * 2, size=32))  # A _q15 is 16-bit
        outputs['model'].write(to_bytes(16, size=8))                # bitwidth
        outputs['model'].write(to_bytes(Constants.SLOT_TEST_SET, size=8))     # slot
        outputs['model'].write(to_bytes(input_channel, size=16))    # tile_c
        # extend_dims
        outputs['model'].write(to_bytes(1))
        outputs['model'].write(to_bytes(input_channel))
        outputs['model'].write(to_bytes(dimX))
        outputs['model'].write(to_bytes(dimY))
    else:
        assert len(params.dims) <= 4
        if params.data_type == onnx.TensorProto.FLOAT:
            if params.float_data:
                float_data = params.float_data
            else:
                float_data = list(map(lambda t: t[0], struct.iter_unpack('f', params.raw_data)))
            data_len = len(float_data)
            assert data_len > 0
            slot = select_parameters_slot(data_len * 2)
            outputs['model'].write(to_bytes(slot.offset, size=32))  # params_offset
            outputs['model'].write(to_bytes(data_len * 2, size=32))  # A _q15 is 16-bit
            if params.name in conv_param_names:
                print(f'Reorder conv param {params.name}')
                float_data, _ = nchw2nhwc(float_data, params.dims)
            for param in float_data:
                slot.target.write(to_bytes(_Q15(param / config['scale'])))
                slot.offset += 2
            outputs['model'].write(to_bytes(16, size=8)) # bitwidth
        elif params.data_type == onnx.TensorProto.INT64:
            data_len = len(params.int64_data)
            slot = select_parameters_slot(data_len * 8)
            outputs['model'].write(to_bytes(slot.offset, size=32))  # params_offset
            outputs['model'].write(to_bytes(data_len * 8, size=32))
            for param in params.int64_data:
                slot.target.write(to_bytes(param, size=64))
                slot.offset += 8
            outputs['model'].write(to_bytes(64, size=8)) # bitwidth
        else:
            assert False
        outputs['model'].write(to_bytes(slot.slot_id, size=8))  # slot
        if len(params.dims) == 4:
            tile_c = params.dims[1]
        else:
            tile_c = 0
        outputs['model'].write(to_bytes(tile_c, size=16))       # tile_c
        print('dims = {}, length = {}'.format(params.dims, data_len))
        for dim in params.dims:
            outputs['model'].write(to_bytes(dim))
        # dims are always 4 uint16_t's in C++
        for _ in range(4 - len(params.dims)):
            outputs['model'].write(to_bytes(0))

    # common to input and non-inputs
    outputs['model'].write(to_bytes(0, size=8))                 # flags
    for _ in range(3):
        outputs['model'].write(to_bytes(0, size=8))             # extra_info
    outputs['model'].write(to_bytes(config['scale']))           # scale
    for _ in range(2):
        outputs['model'].write(to_bytes(0, size=8))             # dummy

# Placeholder for ParameterInfo of intermediate values
for idx, n in enumerate(nodes):
    outputs['model'].write(to_bytes(0, size=32))  # params_offset
    outputs['model'].write(to_bytes(0, size=32))  # params_len
    outputs['model'].write(to_bytes(0, size=8))  # bitwidth
    outputs['model'].write(to_bytes(0, size=8))  # slot
    outputs['model'].write(to_bytes(0, size=16))  # tile_c
    for _ in range(4):  # dims[4]
        outputs['model'].write(to_bytes(0))
    outputs['model'].write(to_bytes(0, size=8))     # flags
    for _ in range(3):
        outputs['model'].write(to_bytes(0, size=8)) # extra_info
    outputs['model'].write(to_bytes(config['scale']))   # scale
    for _ in range(2):
        outputs['model'].write(to_bytes(0, size=8))             # dummy

inputs_data.seek(0)
outputs['model'].write(inputs_data.read())

for idx, im in enumerate(images):
    # load_data returns NCHW
    for idx_c in range(im.shape[1]):
        for idx_h in range(im.shape[2]):
            for idx_w in range(im.shape[3]):
                outputs['samples'].write(to_bytes(_Q15(im[0, idx_c, idx_h, idx_w] / config['scale'])))
    try:
        import cv2
        # Restore conanical image format (H, W, C)
        im = np.squeeze(im * 256)
        if args.config == 'mnist':
            im = np.expand_dims(im, axis=-1)
            im = 255 - im
        cv2.imwrite(f'images/test{idx:02d}.png', im)
    except ImportError:
        pass

for label in labels:
    outputs['labels'].write(to_bytes(label, size=8))

with open('images/ans.txt', 'w') as f:
    f.write(' '.join(map(str, labels)))

outputs['counters'].write(b'\0' * (4 * Constants.COUNTERS_LEN + 2))

with open('data.cpp', 'w') as output_c, open('data.h', 'w') as output_h:
    output_h.write('''
#pragma once

#include <stdint.h>

struct ParameterInfo;
struct Model;

''')
    for item in itertools.chain(dir(Constants), config.keys()):
        if hasattr(Constants, item):
            if item.startswith('__'):
                continue
            val = getattr(Constants, item)
        else:
            val = config[item]
            if not isinstance(val, (int, float)):
                continue
        output_h.write(f'#define {item.upper()} {val}\n')

    if not args.without_progress_embedding:
        output_h.write('''
#define WITH_PROGRESS_EMBEDDING
''')
    output_c.write('''
#include "data.h"
#include "cnn_common.h"
#include "platform.h"
''')

    # ops
    keys = list(ops.keys())
    output_h.write('\n')
    for idx, op in enumerate(keys):
        output_h.write(f'#define {op} {idx}\n')

    output_c.write('uint8_t expected_inputs_len[] = {')
    for op in keys:
        output_c.write(f'{ops[op][0]}, ')
    output_c.write('};\n\n')

    for op in keys:
        output_h.write('void alloc_{}(struct Model *model, struct ParameterInfo *input[], struct ParameterInfo *output, uint16_t flags);\n'.format(op.lower()))
        output_h.write('void handle_{}(struct Model *model, struct ParameterInfo *input[], struct ParameterInfo *output, uint16_t flags);\n'.format(op.lower()))
    output_c.write('handler handlers[] = {\n')
    for op in keys:
        output_c.write(f'    handle_{op},\n'.lower())
    output_c.write('};\n')
    output_c.write('allocator allocators[] = {\n')
    for op in keys:
        output_c.write(f'    alloc_{op},\n'.lower())
    output_c.write('};\n')
    for op in keys:
        if ops[op][1]:
            output_c.write(f'void alloc_{op.lower()}(Model *model, ParameterInfo *[], struct ParameterInfo *output, uint16_t) {{\n')
            output_c.write('    model->slot_users[output->slot] = model->layer_idx;\n')
            output_c.write('}\n')

    # data
    for idx, name in enumerate(other_flags):
        output_h.write(f'#define {name} {2**idx}\n')

    def hex_str(arr):
        return '  ' + ', '.join([f'0x{num:02x}' for num in arr]) + ',\n'

    def define_var(var_name, data):
        output_h.write(f'''
extern uint8_t *{var_name};
#define {var_name.upper()}_LEN {len(data)}
''')
        # #define with _Pragma seems to be broken :/
        if var_name == 'parameters_data':
            section = 'nvm'
        else:
            section = 'nvm2'
        output_c.write(f'''
#ifdef NEED_DATA_VARS
#pragma DATA_SECTION(".{section}")
uint8_t _{var_name}[{len(data)}] = {{
''')
        n_pieces, remaining = divmod(len(data), 16)
        for idx in range(n_pieces):
            output_c.write(hex_str(data[idx*16:(idx+1)*16]))
        if remaining:
            output_c.write(hex_str(data[len(data) - remaining:len(data)]))
        output_c.write(f'''}};
uint8_t *{var_name} = _{var_name};
#endif
''')

    for var_name, data_obj in outputs.items():
        if var_name in ('samples', 'labels'):
            continue
        var_name += '_data'
        data_obj.seek(0)
        define_var(var_name, data_obj.read())

    outputs['samples'].seek(0)
    samples_data = outputs['samples'].read()
    outputs['labels'].seek(0)
    labels_data = outputs['labels'].read()

    define_var('samples_data', samples_data[:len(samples_data)//len(labels)])
    define_var('labels_data', labels_data[:len(labels_data)//len(labels)])

with open('nvm.bin', 'wb') as f:
    f.write(config['nvm_size'] * b'\0')
    f.seek(config['num_slots'] * config['intermediate_values_size'])
    for data_obj in outputs.values():
        data_obj.seek(0)
        f.write(data_obj.read())
        needed_nvm_size = f.tell()
        assert needed_nvm_size < config['nvm_size'], f'Need NVM size {needed_nvm_size}'
