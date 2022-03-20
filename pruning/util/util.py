from __future__ import print_function
import os
import sys
import csv
import json
import math
import types
import torch
import torch.nn as nn
import numpy as np
import math
import models
import copy
import logging
import subprocess

os.environ['CUDA_VISIBLE_DEVICES'] = '0'

from scipy.sparse import csr_matrix, bsr_matrix
from torchsummary import summary
from torch.autograd import Variable
from itertools import chain
from collections import OrderedDict
from config import config
from tqdm import tqdm, trange

cwd = os.getcwd()
sys.path.append(cwd+'/../')

def set_logger(args):
    # Set logger info
    path = args.prune+'/'+args.arch
    subprocess.call('mkdir -p logs', shell=True)
    subprocess.call('mkdir -p logs/'+args.prune, shell=True)
    subprocess.call('mkdir -p logs/'+path, shell=True)
    logger = logging.getLogger(__name__)
    formatter = logging.Formatter(
        '[%(levelname)s %(asctime)s %(module)s:%(lineno)d] %(message)s',
        datefmt='%Y%m%d %H:%M:%S')
    ch = logging.StreamHandler()
    fh = logging.FileHandler('logs/'+path+'/stage_'+str(args.stage)+'.txt')
    ch.setFormatter(formatter)
    fh.setFormatter(formatter)
    level = args.debug
    if level == 1:
        # debug
        logger.setLevel(logging.DEBUG)
        ch.setLevel(logging.DEBUG)
        fh.setLevel(logging.DEBUG)
    elif level == 0:
        # info
        logger.setLevel(logging.INFO)
        ch.setLevel(logging.INFO)
        fh.setLevel(logging.INFO)

    # logger.addHandler(ch)
    logger.addHandler(fh)
    return logger

# https://github.com/sksq96/pytorch-summary/blob/master/torchsummary/torchsummary.py
def activation_shapes(model, input_size, batch_size=-1, device=torch.device('cuda:0'), dtypes=None):
    if not torch.cuda.is_available():
        device = torch.device('cpu')
    if dtypes == None:
        dtypes = [torch.FloatTensor]*len(input_size)

    summary_str = ''

    def register_hook(module):
        def hook(module, input, output):
            class_name = str(module.__class__).split(".")[-1].split("'")[0]
            module_idx = len(summary)

            m_key = "%s-%i" % (class_name, module_idx + 1)
            summary[m_key] = OrderedDict()
            summary[m_key]["input_shape"] = list(input[0].size())
            summary[m_key]["input_shape"][0] = batch_size
            if isinstance(output, (list, tuple)):
                summary[m_key]["output_shape"] = [
                    [-1] + list(o.size())[1:] for o in output
                ]
            else:
                summary[m_key]["output_shape"] = list(output.size())
                summary[m_key]["output_shape"][0] = batch_size

            params = 0
            if hasattr(module, "weight") and hasattr(module.weight, "size"):
                params += torch.prod(torch.LongTensor(list(module.weight.size())))
                summary[m_key]["trainable"] = module.weight.requires_grad
            if hasattr(module, "bias") and hasattr(module.bias, "size"):
                params += torch.prod(torch.LongTensor(list(module.bias.size())))
            summary[m_key]["nb_params"] = params

        if (
            not isinstance(module, nn.Sequential)
            and not isinstance(module, nn.ModuleList)
        ):
            hooks.append(module.register_forward_hook(hook))

    # multiple inputs to the network
    if isinstance(input_size, tuple):
        input_size = [input_size]

    # batch_size of 2 for batchnorm
    x = [torch.rand(2, *in_size).type(dtype).to(device=device)
         for in_size, dtype in zip(input_size, dtypes)]

    # create properties
    summary = OrderedDict()
    hooks = []

    # register hook
    model.apply(register_hook)

    # make a forward pass
    # print(x.shape)
    model(*x)

    # remove these hooks
    for h in hooks:
        h.remove()

    names = []
    shapes = []
    valid_layer = ("Conv2d", "Linear")
    for layer in summary:
        if layer[:layer.find('-')] in valid_layer:
            names.append(layer)
            shapes.append(summary[layer]["output_shape"])

    return names, shapes

def print_layer_info(model):
    index = 0
    print()
    for m in model.modules():
        if hasattr(m, 'alpha'):
            print('MaskLayer', index, ':',
                    m.alpha.data.nelement()-int(m.alpha.data.eq(1.0).sum()), 'of',
                    m.alpha.data.nelement(), 'is blocked')
            index += 1
    print()
    return

def print_args(args):
    print('\n==> Setting params:')
    for key in vars(args):
        print(key, ':', getattr(args, key))
    print('====================\n')
    return

def lowering(tensor, shape):
    origin_size = shape
    matrix = tensor.reshape(shape[0], -1)
    return matrix

def im2col(arr, dims):
    arr = np.reshape(arr, (dims[0], -1))
    return arr

def nchw2nwhc_without_flatten(arr):
    arr = np.transpose(arr, axes=(0, 3, 2, 1))  # NCHW -> NWHC
    return arr

def xxxx2xcxxx(arr, config, dims):
    chunk_len = dims[1] # c
    arr = im2col(arr, dims)
    new_arr = []

    for row in arr:
        # avoid omiting 0 when transform the matrix to csr
        row = row + 1
        lists = [np.array(row[i : i + chunk_len]) for i in range(0, len(row), chunk_len)]
        lists = np.array(lists)
        group_size = (dims[2] * dims[3], config['group'][1])
        bsr = csr_matrix(lists).tobsr(group_size)
        new_row = []
        for data in bsr.data:
            data = data.flatten() - 1
            new_row.extend(data)
        new_arr.append(new_row)
    return np.array(new_arr)

def toBSR(matrix, group):
    bsr = csr_matrix(matrix).tobsr(group)
    return bsr

def to_onnx(source, name, args):
    if args.arch == 'LeNet_5':
        model = models.LeNet_5(None)
        input_shape = (1,28,28)
        dummy_input = Variable(torch.randn(1, 1, 28, 28))
    elif args.arch == 'mnist':
        model = models.MNIST(None)
        input_shape = (1,28,28)
        dummy_input = Variable(torch.randn(1, 1, 28, 28))
    elif args.arch == 'SqueezeNet':
        model = models.SqueezeNet(None)
        input_shape = (1,32,32)
        dummy_input = Variable(torch.randn(1, 3, 32, 32))
    # https://discuss.pytorch.org/t/missing-keys-unexpected-keys-in-state-dict-when-loading-self-trained-model/22379/14
    state_dict = torch.load(source)['state_dict']
    model.load_state_dict(state_dict)
    converted_name = "./Intermittent_Aware/onnx_models/{}.onnx".format(args.arch)
    torch.onnx.export(model, dummy_input, converted_name)
    print('Converted model: {}'.format(converted_name))
    return

def load_state(model, state_dict):
    param_dict = dict(model.named_parameters())
    state_dict_keys = state_dict.keys()
    cur_state_dict = model.state_dict()
    for key in cur_state_dict:
        if key in state_dict_keys:
            cur_state_dict[key].copy_(state_dict[key])
        elif key.replace('module.','') in state_dict_keys:
            cur_state_dict[key].copy_(state_dict[key.replace('module.','')])
        elif 'module.'+key in state_dict_keys:
            cur_state_dict[key].copy_(state_dict['module.'+key])
    return

def nchw2nhwc(arr):
    return arr.permute(0, 2, 3, 1) # NCHW -> NHWC

def nhwc2nchw(arr):
    return arr.permute(0, 3, 1, 2) # NHWC -> NCHW

class MetricsMaker:
    def __init__(self, model, args, output_shapes, cost={'NVM': 100, 'VM': 1}):
        self.model_ = model
        self.args_ = args
        self.output_shapes_ = output_shapes
        self.layer_info_ = []
        self.metrics_ = None
        self.prune_order_ = None
        self.cost_ = cost

    def profile(self):
        metrics = []
        node_idx = 0
        for m in self.model_.modules():
            if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                metric_all = self.profile_node(self.args_, m, self.output_shapes_, node_idx)
                if self.args_.prune == 'intermittent':
                    # XXX: psum jobs only? or all the nvm read/write cost which are incurred by intermittent bebavior ?
                    #metric = metric_all['vm_jobs'] + metric_all['vm_read_psum'] + metric_all['vm_write_psum'] + metric_all['nvm_jobs']
                    metric = metric_all['vm_jobs'] + metric_all['vm_write_psum'] + metric_all['pruned_ofm_element']
                    metrics.append(metric)
                elif self.args_.prune == 'energy':
                    metric = metric_all['nvm_access'] * self.cost_['NVM'] + metric_all['vm_access'] * self.cost_['VM']
                    metrics.append(metric)
                node_idx += 1
        order = sorted(range(len(metrics)), key=lambda k : metrics[k])
        self.metrics_ = metrics
        self.prune_order_ = order
        self.saveInfo()

    def getMetrics(self):
        return self.metrics_, self.prune_order_

    def getLayerInfo(self):
        return self.layer_info_

    def saveInfo(self):
        with open('logs/'+self.args_.prune+'/'+self.args_.arch+'/stage_'+str(self.args_.stage)+'.csv', 'w') as csvfile:
            fields = [name for name in self.layer_info_[0]]
            writer = csv.DictWriter(csvfile, fieldnames=fields)
            writer.writeheader()
            for item in self.layer_info_:
                row = {}
                for field in fields:
                    row[field] = item[field]
                writer.writerow(row)

        with open('logs/'+self.args_.prune+'/'+self.args_.arch+'/stage_'+str(self.args_.stage)+'_pruning_order.csv', 'w') as csvfile:
            fields = [str(idx) for idx, name in enumerate(self.prune_order_)]
            writer = csv.DictWriter(csvfile, fieldnames=fields)
            writer.writeheader()
            row = {}
            for idx, item in enumerate(self.prune_order_):
                row[str(idx)] = item
            writer.writerow(row)

    def profile_node(self, args, node, output_shapes, node_idx):
        layer_config = config[args.arch][node_idx]
        shape = node.weight.data.shape
        matrix = node.weight.data
        matrix = matrix.reshape(shape[0], -1).cpu()
        # group: [n_filters, n_channels]
        if isinstance(node, nn.Conv2d):
            if self.args_.prune_shape == 'channel':
                # rows: the number of filter groups
                # cols: the number of input_tile_c
                n_row, n_col = layer_config['group'][0], layer_config['group'][1] * layer_config['filter'][2] * layer_config['filter'][3]
            elif self.args_.prune_shape == 'vector':
                # rows: the number of filter groups
                # cols: input_tile_c * K * K
                n_row, n_col = layer_config['group'][0], layer_config['group'][1]
                matrix = nchw2nwhc_without_flatten(matrix.view(shape))
                matrix = xxxx2xcxxx(matrix, layer_config, shape)
            group_size = (n_row, n_col)
            width = n_col * n_row
        elif isinstance(node, nn.Linear):
            # rows: the number of input_tile_c
            # cols: the number of filter filters
            n_row, n_col = layer_config['group'][1], layer_config['group'][0]
            group_size = (n_row, n_col)
            width = n_col * n_row
            matrix = np.transpose(matrix)

        bsr = toBSR(matrix, group_size)
        data = bsr.data
        cols = bsr.indices
        rows = bsr.indptr

        nvm_jobs = 0
        nvm_read_weights = 0
        nvm_read_inputs = 0
        nvm_read_psum = 0
        nvm_write_psum = 0
        pruned_ofm_element = 0
        vm_jobs = 0
        vm_read_psum = 0
        vm_write_psum = 0
        vm_read_weights = 0
        vm_read_inputs = 0
        op_type = None

        logger_.debug("Node: {}".format(node_idx))
        if isinstance(node, nn.Linear):
            op_type = 'FC'
            # input stationary
            for i in range(1, len(rows)):
                if rows[i] - rows[i - 1] != 0:
                    nvm_read_inputs += n_row
            nvm_read_weights += len(cols) * n_row * n_col
            nvm_jobs += layer_config['output'][0] * layer_config['output'][1] * layer_config['output'][2] * layer_config['output'][3]
            vm_read_inputs += len(cols) * n_row * n_col
            vm_read_weights += len(cols) * n_row * n_col
            vm_read_psum += 2 * (len(rows) - 1) * layer_config['output'][1]
            vm_write_psum += (len(rows) - 1) * layer_config['output'][1]
            vm_jobs += len(cols) * layer_config['group'][0]
        elif isinstance(node, nn.Conv2d):
            op_type = 'CONV'
            # weight stationary
            # XXX: channel shape should be rechecked
            if self.args_.prune_shape == 'channel':
                per_input_nvm_read_for_a_weight_group = math.ceil(layer_config['tile']['weight'][3] / layer_config['stride']) # e.g. 5 / 1 = 5
            elif self.args_.prune_shape == 'vector':
                per_input_nvm_read_for_a_weight_group = 1
            n_output_tile_per_weight_group = \
                math.ceil(layer_config['output'][2] / layer_config['tile']['output'][2]) * \
                math.ceil(layer_config['output'][3] / layer_config['tile']['output'][3])
            n_input_tile_c = math.ceil(layer_config['input'][1] / layer_config['tile']['input'][1])

            nvm_read_weights += len(cols) * n_row * n_col * n_output_tile_per_weight_group
            nvm_jobs += layer_config['output'][0] * layer_config['output'][1] * layer_config['output'][2] * layer_config['output'][3]
            vm_read_inputs += len(cols) * n_row * n_col * layer_config['output'][2] * layer_config['output'][3]
            vm_read_weights += len(cols) * n_row * n_col * layer_config['output'][2] * layer_config['output'][3]
            for i in range(1, len(rows)):
                n_tile_c = rows[i] - rows[i - 1]
                if n_tile_c:
                    tile_c_set = set()
                    for idx in range(rows[i - 1], rows[i]):
                        tile_c_set.add(int(cols[idx] / (layer_config['filter'][2] * layer_config['filter'][3])))
                    nvm_read_inputs += len(tile_c_set) * layer_config['group'][1] * \
                        (layer_config['output'][2]) * \
                        (layer_config['output'][3] + layer_config['pads'][1] + layer_config['pads'][3]) * \
                        n_output_tile_per_weight_group
                    vm_read_psum += 2 * n_tile_c * n_row * \
                        layer_config['output'][2] * layer_config['output'][3]
                    vm_write_psum += n_tile_c * n_row * \
                        layer_config['output'][2] * layer_config['output'][3]
                    vm_jobs += n_tile_c * n_row * \
                        layer_config['output'][2] * layer_config['output'][3]
                    nvm_read_psum += n_input_tile_c  * \
                        layer_config['output'][2] * layer_config['output'][3]
                    nvm_write_psum += n_input_tile_c  * \
                        layer_config['output'][2] * layer_config['output'][3]
                else:
                    pruned_ofm_element += n_row * layer_config['output'][2] * layer_config['output'][3]

        logger_.debug('group size: {}'.format(group_size))
        vm_access = (vm_jobs + vm_read_inputs + vm_read_weights + vm_read_psum + vm_write_psum)
        nvm_access = (nvm_jobs + nvm_read_inputs + nvm_read_weights + nvm_read_psum + nvm_write_psum)
        self.layer_info_.append({
            'node': node_idx,
            'op_type': op_type,
            'nvm_read_inputs': nvm_read_inputs,
            'nvm_read_weights': nvm_read_weights,
            'nvm_jobs': nvm_jobs,
            'vm_jobs': vm_jobs,
            'vm_read_weights': vm_read_weights,
            'vm_read_inputs': vm_read_inputs,
            'vm_read_psum': vm_read_psum,
            'vm_write_psum': vm_write_psum,
            'nvm_access': nvm_access,
            'vm_access': vm_access,
            'pruned_ofm_element': pruned_ofm_element,
        })
        return self.layer_info_[-1]

def prune_weight_layer(m, mask):
    if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
        m.weight.data[mask] = 0
    return m.weight.data

def prune_weight(model):
    logger_.info('Start pruning weight...')
    index = 0
    for m in model.modules():
        if isinstance(m, nn.Linear):
            m.weight.data[model.weights_pruned[index]] = 0
            index += 1
        elif isinstance(m, nn.Conv2d):
            m.weight.data[model.weights_pruned[index]] = 0
            index += 1
    logger_.info('Finish pruning weight.')
    return model

class SimulatedAnnealing():
    def __init__(self, model, start_temp, stop_temp, cool_down_rate, perturbation_magnitude, target_sparsity, args, evaluate_function, input_shape, output_shapes, mask_maker, metrics_maker):
        self.model_ = model
        self.start_temp_ = start_temp
        self.stop_temp_ = stop_temp
        self.cool_down_rate_ = cool_down_rate
        self.perturbation_magnitude_ = perturbation_magnitude
        self.target_sparsity_ = target_sparsity
        self.args_ = args
        self.evaluator_ = evaluate_function

        self.sparsities_ = None

        self.cur_perf_ = -np.inf
        self.best_perf_ = -np.inf
        self.best_sparsities_ = []
        self.search_history_ = []

        self.input_shape_ = input_shape
        self.output_shapes_ = output_shapes
        self.mask_maker_ = mask_maker
        self.metrics_maker_ = metrics_maker
        self.start()

    def get_n_node(self):
        cnt = 0
        for m in self.model_.modules():
            if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                cnt += 1
        return cnt

    def get_sparsities(self):
        return self.best_sparsities_

    def rescale_sparsities(self, sparsities, target_sparsity):
        metrics, order = self.metrics_maker_.getMetrics()
        sparsities = sorted(sparsities)
        total_weight = 0
        total_weight_pruned = 0
        for i in range(len(sparsities)):
            node_idx = order[i]
            node_weights = self.model_.weights_pruned[node_idx]
            n_node_weights = np.prod(node_weights.shape)
            total_weight += n_node_weights
            total_weight_pruned += int(n_node_weights * sparsities[i])
        scale = target_sparsity / (total_weight_pruned / total_weight)
        sparsities = np.asarray(sparsities) * scale
        # check the result of rescalling
        total_weight_pruned_test = 0
        for i in range(len(sparsities)):
            node_idx = order[i]
            node_weights = self.model_.weights_pruned[node_idx]
            n_node_weights = np.prod(node_weights.shape)
            total_weight_pruned_test += int(n_node_weights * sparsities[i])
        logger_.debug('Metrics: {}'.format(metrics))
        logger_.debug('Rescale_sparsity: %s', total_weight_pruned_test / total_weight)

        return sparsities

    def init_sparsities(self):
        while True:
            sparsities = sorted(np.random.uniform(0, 1, self.get_n_node()))
            sparsities = self.rescale_sparsities(sparsities, target_sparsity=self.target_sparsity_)

            if sparsities is not None and sparsities[0] >= 0 and sparsities[-1] < 1:
                logger_.info('Gen sparsities: {}'.format(sparsities))
                self.sparsities_ = sparsities
                break

    def generate_perturbations(self):
        '''
        Generate perturbation to the current sparsities distribution.
        Returns:
        --------
        list
            perturbated sparsities
        '''
        logger_.info("Generating perturbations to the current sparsities...")

        # decrease magnitude with current temperature
        magnitude = self.cur_temp_ / self.start_temp_ * self.perturbation_magnitude_
        logger_.info('current perturation magnitude:%s', magnitude)

        while True:
            perturbation = np.random.uniform(-magnitude, magnitude, self.get_n_node())
            sparsities = np.clip(0, self.sparsities_ + perturbation, None)
            logger_.debug("sparsities before rescalling: {}".format(sparsities))

            sparsities = self.rescale_sparsities(sparsities, target_sparsity=self.target_sparsity_)
            logger_.debug("sparsities after rescalling: {}".format(sparsities))

            if sparsities is not None and sparsities[0] >= 0 and sparsities[-1] < 1:
                logger_.info("Sparsities perturbated:%s", sparsities)
                return sparsities

    def apply_sparsities(self, sparsities):
        pruning_ratios = [0] * self.get_n_node()
        metrics, order = self.metrics_maker_.getMetrics()
        sparsities = sorted(sparsities)
        for i in range(len(order)):
            pruning_ratios[order[i]] = sparsities[i]
        logger_.debug('Pruning order: {}'.format(order))
        logger_.debug('sparsities: {}'.format(sparsities))
        logger_.debug('Pruning ratios: {}'.format(pruning_ratios))
        return pruning_ratios

    def save_search_history(self):
        with open('logs/'+self.args_.prune+'/'+self.args_.arch+'/stage_'+str(self.args_.stage)+'_search_history.csv', 'w') as csvfile:
            writer = csv.DictWriter(csvfile, fieldnames=['sparsity', 'performance', 'pruning_ratios'])
            writer.writeheader()
            for item in self.search_history_:
                writer.writerow({'sparsity': item['sparsity'], 'performance': item['performance'], 'pruning_ratios': item['pruning_ratios']})

    def start(self):
        logger_.info('Starting Simulated Annealing...')
        print('===> Starting Simulated Annealing...')
        it = 0
        self.init_sparsities()

        self.cur_temp_ = self.start_temp_
        while self.cur_temp_ > self.stop_temp_:
            print('\r' + '[Iter {}] cur_temp: {:.2f} | stop_temp: {:.2f} | Loss: {:.4f} | Best perf: {:.4f}'.format(it, self.cur_temp_, self.stop_temp_, self.cur_perf_, self.best_perf_), end='')
            logger_.info('Iter {}:'.format(it))
            logger_.info('Current Temperature: {}'.format(self.cur_temp_))

            while True:
                # generate perturbation
                model_masked = copy.deepcopy(self.model_)
                sparsities_perturbated = self.generate_perturbations()
                config_list = self.apply_sparsities(sparsities_perturbated)
                logger_.info('config_list for Pruner generated: {}'.format(config_list))

                # fast evaluation
                model_masked.weights_pruned = self.mask_maker_.get_masks(config_list)
                model_masked = prune_weight(model_masked)
                evaluation_result = self.evaluator_(model_masked, logger_)

                self.search_history_.append(
                    {'sparsity': self.sparsities_, 'performance': evaluation_result, 'pruning_ratios': config_list})

                evaluation_result *= -1

                # if better evaluation result, then accept the perturbation
                if evaluation_result > self.cur_perf_:
                    self.cur_perf_ = evaluation_result
                    self.sparsities_ = sparsities_perturbated

                    # save best performance and best params
                    if evaluation_result > self.best_perf_:
                        logger_.info('updating best model...')
                        self.best_perf_ = evaluation_result
                        self.best_sparsities_ = config_list

                        # save the overall best masked model
                        self.bound_model = model_masked
                    break
                # if not, accept with probability e^(-deltaE/current_temperature)
                else:
                    delta_E = np.abs(evaluation_result - self.cur_perf_)
                    probability = math.exp(-1 * delta_E /
                                           self.cur_temp_)
                    if np.random.uniform(0, 1) < probability:
                        logger_.info('Escape local optimized value.')
                        self.cur_perf_ = evaluation_result
                        self.sparsities_ = sparsities_perturbated
                        break
            # cool down
            self.cur_temp_ *= self.cool_down_rate_
            it += 1
        logger_.info('Finish simulating anealing.')
        self.save_search_history()

class MaskMaker():
    def __init__(self, model, args, input_shape, metrics_maker):
        self.args_ = args
        self.model_ = model
        self.input_shape = input_shape
        _, self.output_shapes_ = activation_shapes(model, input_shape)
        self.weight_masks_ = []
        self.metrics_maker_ = metrics_maker
        self.pruned_ratios_ = self.getPrunedRatios(model.weights_pruned, self.output_shapes_)
        self.generate_masks()

    def getPrunedRatios(self, masks, output_shapes):
        if masks == None or masks == []:
            return [0] * len(output_shapes)
        pruned_ratios = []
        for i in range(len(masks)):
            n_pruned = int(masks[i].sum())
            total = int(masks[i].nelement())
            pruned_ratios.append(float(n_pruned / total))
        logger_.debug('Pruned ratios: {}'.format(pruned_ratios))
        return pruned_ratios

    def setPruningRatios(self, model, candidates, output_shapes):
        # https://gist.github.com/georgesung/ddb3a0b0412513d8811696293d8b1771
        pruning_ratios = [0] * len(output_shapes)
        metrics, order = self.metrics_maker_.getMetrics()
        for i in range(len(order)):
            pruning_ratios[order[i]] = candidates[i]
        logger_.debug("Metrics : {}".format(metrics))
        logger_.debug("Pruning Order: {}".format(order))
        logger_.debug('pruning_ratios: {}'.format(pruning_ratios))
        return pruning_ratios

    def criteria(self, tmp_pruned, pruning_ratio, pruned_ratio):
        values = tmp_pruned.pow(2.0).mean(1, keepdim=True).pow(0.5)
        tmp_val = np.array(values.tolist()).flatten()
        n_values = len(tmp_val)
        sorted_idx = sorted(range(len(tmp_val)), key = lambda k : tmp_val[k])[: int(n_values * pruning_ratio)]
        mask = np.zeros(tmp_val.shape, dtype=bool)
        mask[sorted_idx] = True
        mask = np.reshape(mask, values.shape)
        return torch.tensor(mask).expand(tmp_pruned.shape)

    def generate_masks(self):
        names, output_shapes = activation_shapes(self.model_, self.input_shape)
        pruned_ratios = self.pruned_ratios_
        pruning_ratios = self.setPruningRatios(self.model_, self.args_.candidates_pruning_ratios, output_shapes)
        self.generate_masks_(pruning_ratios, pruned_ratios)

    def to_group_shape(self, tensor, group_size, m):
        if self.args_.prune_shape == 'channel':
            # group size: [2, 1], origin: [16, 8, 5, 5]
            # [16, 8, 5, 5] -> [16, 8, 25] -> [16, 200] -> [64, 2, 25] -> [64, 50]
            tensor = tensor.reshape(tensor.shape[0], -1).cpu() # ->[16, 200]
            bsr = toBSR(tensor, group_size) # -> [64, 2, 25]
            data = bsr.data
            self.cols = bsr.indices
            self.rows = bsr.indptr
            self.bsr_shape = data.shape
            self.matrix_shape = tensor.size()
            data = data.reshape(data.shape[0], -1) # [64, 50]
        elif self.args_.prune_shape == 'vector':
            # group size: [2, 1], origin: [16, 8, 5, 5]
            # [16, 8, 5, 5] -> [16, 5, 5, 8] -> [16, 5x5x8] -> [200, 2, 8] -> [200, 16]
            if isinstance(m, nn.Conv2d):
                tensor = nchw2nhwc(tensor)
                self.nhwc_shape = tensor.shape
            tensor = tensor.reshape(tensor.shape[0], -1).cpu()
            bsr = toBSR(tensor, group_size)
            data = bsr.data
            self.cols = bsr.indices
            self.rows = bsr.indptr
            self.bsr_shape = data.shape # [200, 2, 8]
            self.matrix_shape = tensor.size() # [16, 5x5x8]
            data = data.reshape(data.shape[0], -1) # [200, 16]
        return torch.tensor(data)

    def recover_shape(self, tensor, original_size, m):
        if self.args_.prune_shape == 'channel':
            # group size: [2, 1], target: [16, 8, 5, 5], tensor: [64, 50]
            # [64, 50] -> [64, 2, 25] -> [16, 200] -> [16, 8, 5, 5]
            tensor = tensor.cpu().detach().numpy()
            tensor = tensor.reshape(self.bsr_shape[0], self.bsr_shape[1], -1)# -> [64, 2, 25]
            origin_matrix = torch.tensor(bsr_matrix((tensor, self.cols, self.rows), shape=self.matrix_shape).toarray()) # ->[16, 200]
            origin_matrix = origin_matrix.view(original_size) # -> [16, 8, 5, 5]
        elif self.args_.prune_shape == 'vector':
            # [200, 16] -> [200, 2, 8] -> [16, 200] -> [16, 5, 5, 8] -> [16, 8, 5, 5]
            tensor = tensor.cpu().detach().numpy()
            tensor = tensor.reshape(self.bsr_shape[0], self.bsr_shape[1], -1)# -> [200, 2, 8]
            origin_matrix = torch.tensor(bsr_matrix((tensor, self.cols, self.rows), shape=self.matrix_shape).toarray()) # ->[16, 200]
            if isinstance(m, nn.Conv2d):
                origin_matrix = origin_matrix.view(self.nhwc_shape) # -> [16, 5, 5, 8]
                origin_matrix = nhwc2nchw(origin_matrix) # -> [16, 8, 5, 5]
            elif isinstance(m, nn.Linear):
                origin_matrix = origin_matrix.view(original_size)
        return origin_matrix

    def generate_masks_(self, pruning_ratios, pruned_ratios=[]):
        logger_.info('Start generating masks ...')
        node_idx = 0
        self.weight_masks_ = []
        if len(pruned_ratios) == 0:
            pruned_ratios = [0] * len(self.model_.weights_pruned)
        for m in self.model_.modules():
            if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                layer_config = config[self.args_.arch][node_idx]
                tmp_pruned = m.weight.data.clone()
                original_size = tmp_pruned.size()
                logger_.debug('original_size: {}'.format(original_size))
                if isinstance(m, nn.Linear):
                    group_size = (layer_config['group'][0], layer_config['group'][1])
                elif isinstance(m, nn.Conv2d):
                    if self.args_.prune_shape == 'channel':
                        group_size = (layer_config['group'][0], layer_config['group'][1] * layer_config['filter'][2] * layer_config['filter'][3])
                    elif self.args_.prune_shape == 'vector':
                        group_size = (layer_config['group'][0], layer_config['group'][1])
                tmp_pruned = self.to_group_shape(tmp_pruned, group_size, m)
                tmp_pruned = self.criteria(tmp_pruned, pruning_ratios[node_idx], pruned_ratios[node_idx])
                logger_.debug('After pruning: {}'.format(tmp_pruned.size()))
                tmp_pruned = tmp_pruned.reshape(tmp_pruned.shape[0], -1)
                tmp_pruned = self.recover_shape(tmp_pruned, original_size, m)
                self.weight_masks_.append(tmp_pruned)
                node_idx += 1
                logger_.info('Finish generating node {} mask.\n'.format(node_idx))
        self.weight_masks_ = [torch.logical_or(x, y) for x, y in zip(self.weight_masks_, self.model_.weights_pruned)]
        logger_.info('Finish generating masks.')

    def get_masks(self, sparsities_perturbated=[]):
        if len(sparsities_perturbated) == 0:
            logger_.info('Get the mask without perturbating.')
            return self.weight_masks_
        logger_.info('Get the mask with perturbating.')
        self.generate_masks_(sparsities_perturbated, self.pruned_ratios_)
        return self.weight_masks_

    @staticmethod
    def gen_masks(model):
        masks = []
        for m in model.modules():
            if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                shape = m.weight.data.size()
                masks.append(torch.zeros(shape, dtype=torch.bool))
        return masks

class ADMMPruner():
    def __init__(self, model, args, trainer, criterion, optimizer, input_shape, sparsities_maker, mask_maker, row=1e-4, n_iterations=5, n_training_epochs=10):
        self.model_ = model
        self.args_ = args
        self.trainer_ = trainer
        self.criterion_ = criterion
        self.optimizer_ = optimizer
        self.row_ = row
        self.n_iter_ = n_iterations
        self.n_training_epochs_ = n_training_epochs
        self.mask_maker_ = mask_maker
        self.sparsities_maker_ = sparsities_maker

        self.patch_optimizer(self, self.callback_)

    def callback_(self):
        # callback function to do additonal optimization, refer to the deriatives of Formula (7)
        node_idx = 0
        for m in self.model_.modules():
            if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                m.weight.data -= self.row_ * \
                    (m.weight.data - self.Z[node_idx] + self.U[node_idx])
                node_idx += 1

    def patch_optimizer(self, *tasks):
        def patch_step(old_step):
            def new_step(_, *args, **kwargs):
                for task in tasks:
                    if callable(task):
                        task()
                output = old_step(*args, **kwargs)
                return output
            return new_step
        if self.optimizer_ is not None:
            self.optimizer_.step = types.MethodType(patch_step(self.optimizer_.step), self.optimizer_)

    def projection(self, weight, wrapper):
        wrapper_cpy = copy.deepcopy(wrapper)
        wrapper_cpy.weight.data = weight
        sparsity = self.sparsities_maker_.get_sparsities()
        mask = self.mask_maker_.get_masks(sparsity)
        return prune_weight_layer(wrapper_cpy, mask)

    def compresss(self):
        logger_.info('Start AMDD pruning ...')
        # initiaze Z, U
        # Z_i^0 = W_i^0
        # U_i^0 = 0
        self.Z = []
        self.U = []
        for m in self.model_.modules():
            if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                z = m.weight.data
                self.Z.append(z)
                self.U.append(torch.zeros_like(z))

        # Loss = cross_entropy +  l2 regulization + \Sum_{i=1}^N \row_i ||W_i - Z_i^k + U_i^k||^2
        # optimization iteration
        for k in range(self.n_iter_):
            logger_.info('ADMM iteration : %d', k)

            # step 1: optimize W with AdamOptimizer
            for epoch in trange(1, self.n_training_epochs_+1):
                self.trainer_(self.model_, optimizer=self.optimizer_, criterion=self.criterion_, epoch=epoch, logger=logger_)

            # step 2: update Z, U
            # Z_i^{k+1} = projection(W_i^{k+1} + U_i^k)
            # U_i^{k+1} = U^k + W_i^{k+1} - Z_i^{k+1}
            node_idx = 0
            for m in enumerate(self.model_.modules()):
                if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                    z = m.weight.data + self.U[node_idx]
                    self.Z[node_idx] = self.projection(z, m)
                    torch.cuda.empty_cache()
                    self.U[node_idx] = self.U[node_idx] + m.weight.data - self.Z[node_idx]
        return self.model_

class Prune_Op():
    def __init__(self, model, train_loader, criterion, input_shape, args, evaluate_function, admm_params=None, evaluate=False):
        # args.group: [n_filter, n_channel]
        global logger_
        logger_ = set_logger(args)
        self.train_loader = train_loader
        self.criterion = criterion
        self.args = args
        self.input_shape = input_shape
        self.names, self.output_shapes = activation_shapes(model, input_shape)
        if model.weights_pruned == None:
            model.weights_pruned = MaskMaker.gen_masks(model)
        self.metrics_maker = MetricsMaker(model=model, args=args, output_shapes=self.output_shapes)
        self.metrics_maker.profile()
        self.mask_maker = MaskMaker(model, args, input_shape, metrics_maker=self.metrics_maker)
        if args.sa:
            self.sparsities_maker = SimulatedAnnealing(model, start_temp=100, stop_temp=20, cool_down_rate=0.9, perturbation_magnitude=0.35, target_sparsity=0.25, args=args, evaluate_function=evaluate_function, input_shape=self.input_shape, output_shapes=self.output_shapes, mask_maker=self.mask_maker, metrics_maker=self.metrics_maker)
        if not evaluate:
            if args.sa:
                if args.admm and admm_params != None:
                    pruner = ADMMPruner(model=model, args=args, trainer=admm_params['train_function'], criterion=admm_params['criterion'], optimizer=admm_params['optimizer'], input_shape=self.input_shape, mask_maker=self.mask_maker, sparsities_maker=self.sparsities_maker)
                    model = pruner.compresss()
                model.weights_pruned = self.mask_maker.get_masks(self.sparsities_maker.get_sparsities())
            else:
                model.weights_pruned = self.mask_maker.get_masks()

        self.weights_pruned = model.weights_pruned
        self.model = model
        self.print_info(self.weights_pruned)
        self.prune_weight()
        return

    def prune_weight(self):
        index = 0
        for m in self.model.modules():
            if isinstance(m, nn.Linear) or isinstance(m, nn.Conv2d):
                m.weight.data[self.weights_pruned[index]] = 0
                index += 1
                # prune bias if entire filter are pruned
                dims = m.weight.data.shape
                for i in range(dims[0]):
                    ans = torch.sum(m.weight.data[i])
                    if ans == 0:
                        m.bias.data[i] = 0
        return

    @staticmethod
    def print_info(weights_pruned):
        print('\n------------------------------------------------------------------')
        print('- Intermittent-aware weight pruning info:')
        pruned_acc = 0
        total_acc = 0
        for i in range(len(weights_pruned)):
            pruned = int(weights_pruned[i].sum())
            total = int(weights_pruned[i].nelement())
            pruned_acc += pruned
            total_acc += total
            print('- Layer '+str(i)+': '+'{0:10d}'.format(pruned)+' / '+\
                    '{0:10d}'.format(total)+ ' ('\
                    '{0:4.1f}%'.format(float(pruned)/total * 100.0)+\
                    ') weights are pruned')
        print('- Total  : '+'{0:10d}'.format(pruned_acc)+' / '+\
                '{0:10d}'.format(total_acc)+ ' ('\
                '{0:4.1f}%'.format(float(pruned_acc)/total_acc * 100.0)+\
                ') weights are pruned')
        print('------------------------------------------------------------------\n')
        return
