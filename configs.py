from utils import (
    load_data_mnist,
    load_data_cifar10,
    load_data_google_speech,
)

# intermediate_values_size should < 65536, or TI's compiler gets confused
configs = {
    'mnist': {
        'onnx_model': 'data/mnist-8.onnx',
        'scale': 4,
        'input_scale': 4,
        'num_slots': 2,
        'intermediate_values_size': 26000,
        'data_loader': load_data_mnist,
        'n_all_samples': 10000,
        'sample_size': [1, 28, 28],
        'op_filters': 4,
        'first_sample_outputs': [ -1.247997, 0.624493, 8.609308, 9.392411, -13.685033, -6.018567, -23.386677, 28.214134, -6.762523, 3.924627 ],
        'fp32_accuracy': 0.9890,
    },
    'pruned_mnist': {
        'onnx_model': 'pruning/Intermittent_Aware/onnx_models/LeNet_5.onnx',
        'scale': 4,
        'input_scale': 4,
        'num_slots': 2,
        'intermediate_values_size': 26000,
        'data_loader': load_data_mnist,
        'n_all_samples': 10000,
        'sample_size': [1, 28, 28],
        'op_filters': 4,
        # 'first_sample_outputs': [-0.603953, -0.099769, -0.226438, 0.090364, 0.235120, -0.055192, -0.766948, 0.888632, 0.108483, 0.441478],
        'first_sample_outputs': [-0.556942, -0.221270, -0.297040, -0.065473, 0.153532, 0.105087, -0.488914, 0.932622, 0.297344, 0.325804],
        'fp32_accuracy': 0.9919,
    },
    'cifar10': {
        'onnx_model': 'data/squeezenet_cifar10.onnx',
        'scale': 2,
        'input_scale': 4,
        'num_slots': 3,
        'intermediate_values_size': 65000,
        'data_loader': load_data_cifar10,
        'n_all_samples': 10000,
        'sample_size': [32, 32, 3],
        'op_filters': 4,
        'first_sample_outputs': [ 4.895500, 4.331344, 4.631835, 11.602396, 4.454658, 10.819544, 5.423588, 6.451203, 5.806091, 5.272837 ],
        'fp32_accuracy': 0.7704,
    },
    'kws': {
        'onnx_model': 'data/KWS-DNN_S.onnx',
        'scale': 1,
        'input_scale': 120,
        'num_slots': 2,
        'intermediate_values_size': 20000,
        'data_loader': load_data_google_speech,
        'n_all_samples': 4890,
        'sample_size': [25, 10],  # MFCC gives 25x10 tensors
        'op_filters': 4,
        'first_sample_outputs': [ -29.228327, 5.429047, 22.146973, 3.142066, -10.448060, -9.513299, 15.832925, -4.655487, -14.588447, -1.577156, -5.864228, -6.609077 ],
        'fp32_accuracy': 0.7983,
    },
}

