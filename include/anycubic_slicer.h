#ifndef ANYCUBIC_SLICER_H
#define ANYCUBIC_SLICER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnycubicSlicerConfig {
    double layer_height_mm;
    uint32_t bottom_layers;
    double bottom_exposure_s;
    double exposure_s;
    double lift_height_mm;
    double lift_speed_mm_s;
    double off_time_s;
    double translation_mm[3];
    double rotation_deg[3];
    double scale[3];
    uint8_t auto_center_xy;
    uint8_t place_on_bed;
    uint8_t supports_enabled;
    uint8_t hollow;
    double xy_offset_mm;
} AnycubicSlicerConfig;

int32_t anycubic_slicer_default_config(AnycubicSlicerConfig *output);
int32_t anycubic_slicer_slice(
    const char *input_path,
    const char *output_path,
    const AnycubicSlicerConfig *configuration);
const char *anycubic_slicer_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
