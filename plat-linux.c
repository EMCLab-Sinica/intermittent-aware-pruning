#include "intermittent-cnn.h"
#include "data.h"
#include "common.h"
#include "debug.h"
#include <stdint.h>
#include <stdio.h>
#include <DSPLib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define NVM_SIZE 256*1024

/* data on NVM, made persistent via mmap() with a file */
uint8_t *intermediate_values;

void run_tests(char *filename) {
    uint8_t label, predicted;
    FILE *test_file = fopen(filename, "r");
    uint32_t correct = 0, total = 0;
    while (!feof(test_file)) {
        fscanf(test_file, "|labels ");
        for (uint8_t i = 0; i < 10; i++) {
            int j;
            int ret = fscanf(test_file, "%d", &j);
            if (ret != 1) {
                fprintf(stderr, "fscanf returns %d, pos = %ld\n", ret, ftell(test_file));
                ERROR_OCCURRED();
            }
            if (j == 1) {
                label = i;
                // not break here, so that remaining numbers are consumed
            }
        }
        fscanf(test_file, " |features ");
        for (uint16_t i = 0; i < 28*28; i++) {
            int j;
            int ret = fscanf(test_file, "%d", &j);
            if (ret != 1) {
                fprintf(stderr, "fscanf returns %d, pos = %ld\n", ret, ftell(test_file));
                ERROR_OCCURRED();
            }
            ((int16_t*)parameters_data)[i] = _Q15(1.0 * j / 256 / SCALE);
        }
        fscanf(test_file, "\n");
        my_printf_debug("Test %d\n", total);
        run_model(&predicted);
        total++;
        if (label == predicted) {
            correct++;
        }
        my_printf_debug("%d %d\n", label, predicted);
        reset_model();
    }
    my_printf("correct=%d total=%d rate=%f\n", correct, total, 1.0*correct/total);
    fclose(test_file);
}

int main(int argc, char* argv[]) {
    int nvm_fd, ret = 0;
    uint8_t *nvm;

    nvm_fd = open("nvm.bin", O_RDWR);
    nvm = mmap(NULL, NVM_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, nvm_fd, 0);
    if (nvm == MAP_FAILED) {
        perror("mmap() failed");
        goto exit;
    }
    intermediate_values = nvm;

    if (argc >= 3) {
        printf("Usage: %s [test filename]\n", argv[0]);
        ret = 1;
    } else if (argc == 2) {
        run_tests(argv[1]);
    } else {
        ret = run_model(NULL);
    }

exit:
    close(nvm_fd);
    return ret;
}