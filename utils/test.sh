set -e
set -x

# preparation
cmake_args=""
run_args=""

pushd ARM-CMSIS && ./download-extract-cmsis.sh && popd
pushd TI-DSPLib && ./download-extract-dsplib.sh && popd

cmake_args="$cmake_args -D MY_DEBUG=1"

rounds=100
power_cycle=0.01
if [[ $CONFIG = *mnist* ]]; then
    ./data/download-mnist.sh
fi
if [[ $CONFIG = *cifar10* ]]; then
    ./data/download-cifar10.sh
    rounds=50
    power_cycle=0.02
fi
if [[ $CONFIG = *japari* ]]; then
    power_cycle=$(awk "BEGIN {print $power_cycle+0.01}")
fi

rm -vf nvm.bin
python transform.py $CONFIG
cmake -B build $cmake_args
make -C build
./build/intermittent-cnn $run_args

# Test intermittent running
if [[ ! $CONFIG = *baseline* ]]; then
    rm -vf nvm.bin

    # somehow a large samples.bin breaks intermittent
    # execution - regenerate samples when needed
    if [[ $CONFIG = *all-samples* ]]; then
        python transform.py ${CONFIG/--all-samples/}
    fi

    cmake_args=${cmake_args/MY_DEBUG=1/MY_DEBUG=2}
    cmake -B build $cmake_args
    make -C build
    python ./run-intermittently.py --rounds $rounds --interval $power_cycle --suffix $LOG_SUFFIX --compress ./build/intermittent-cnn
fi
