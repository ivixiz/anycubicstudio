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
    uint8_t supports_auto_generate;
    uint8_t hollow;
    uint32_t support_shape; /* 0 cylinder, 1 tapered, 2 square */
    double support_overhang_angle_deg;
    double support_spacing_mm;
    double support_tip_diameter_mm;
    double support_trunk_diameter_mm;
    double support_base_diameter_mm;
    double support_tip_height_mm;
    double support_base_height_mm;
    double support_penetration_mm;
    double support_min_height_mm;
    double support_max_height_mm;
    uint32_t support_radial_segments;
    double xy_offset_mm;
} AnycubicSlicerConfig;

typedef struct AnycubicSupportPoint {
    double contact_mm[3];
    /* Set to NAN to find the nearest surface/build plate automatically. */
    double base_z_mm;
} AnycubicSupportPoint;

int32_t anycubic_slicer_default_config(AnycubicSlicerConfig *output);
int32_t anycubic_slicer_slice(
    const char *input_path,
    const char *output_path,
    const AnycubicSlicerConfig *configuration);
int32_t anycubic_slicer_slice_with_supports(
    const char *input_path,
    const char *output_path,
    const AnycubicSlicerConfig *configuration,
    const AnycubicSupportPoint *support_points,
    uintptr_t support_point_count);
const char *anycubic_slicer_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
