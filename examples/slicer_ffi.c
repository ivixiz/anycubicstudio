#include "anycubic_slicer.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s MODEL OUTPUT.pwsz\n", argv[0]);
        return 2;
    }

    AnycubicSlicerConfig config;
    if (anycubic_slicer_default_config(&config) != 0) {
        fprintf(stderr, "%s\n", anycubic_slicer_last_error());
        return 1;
    }

    if (anycubic_slicer_slice(argv[1], argv[2], &config) != 0) {
        fprintf(stderr, "%s\n", anycubic_slicer_last_error());
        return 1;
    }
    return 0;
}
