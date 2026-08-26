//! Stable C ABI for embedding the slicer in native applications.

use std::{
    cell::RefCell,
    ffi::{CStr, CString, c_char},
    path::PathBuf,
};

use super::{SlicerConfig, SupportPoint, SupportShape, slice_file};

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AnycubicSlicerConfig {
    pub layer_height_mm: f64,
    pub bottom_layers: u32,
    pub bottom_exposure_s: f64,
    pub exposure_s: f64,
    pub lift_height_mm: f64,
    pub lift_speed_mm_s: f64,
    pub off_time_s: f64,
    pub translation_mm: [f64; 3],
    pub rotation_deg: [f64; 3],
    pub scale: [f64; 3],
    pub auto_center_xy: u8,
    pub place_on_bed: u8,
    pub supports_enabled: u8,
    pub supports_auto_generate: u8,
    pub hollow: u8,
    pub support_shape: u32,
    pub support_overhang_angle_deg: f64,
    pub support_spacing_mm: f64,
    pub support_tip_diameter_mm: f64,
    pub support_trunk_diameter_mm: f64,
    pub support_base_diameter_mm: f64,
    pub support_tip_height_mm: f64,
    pub support_base_height_mm: f64,
    pub support_penetration_mm: f64,
    pub support_min_height_mm: f64,
    pub support_max_height_mm: f64,
    pub support_radial_segments: u32,
    pub xy_offset_mm: f64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AnycubicSupportPoint {
    pub contact_mm: [f64; 3],
    /// Use NaN to find a base automatically; otherwise supplies an exact Z.
    pub base_z_mm: f64,
}

impl Default for AnycubicSlicerConfig {
    fn default() -> Self {
        Self::from(&SlicerConfig::default())
    }
}

impl From<&SlicerConfig> for AnycubicSlicerConfig {
    fn from(config: &SlicerConfig) -> Self {
        Self {
            layer_height_mm: config.print.layer_height_mm,
            bottom_layers: config.print.bottom_layers,
            bottom_exposure_s: config.print.bottom_exposure_s,
            exposure_s: config.print.exposure_s,
            lift_height_mm: config.print.lift_height_mm,
            lift_speed_mm_s: config.print.lift_speed_mm_s,
            off_time_s: config.print.off_time_s,
            translation_mm: config.transform.translation_mm,
            rotation_deg: config.transform.rotation_deg,
            scale: config.transform.scale,
            auto_center_xy: u8::from(config.transform.auto_center_xy),
            place_on_bed: u8::from(config.transform.place_on_bed),
            supports_enabled: u8::from(config.supports.enabled),
            supports_auto_generate: u8::from(config.supports.auto_generate),
            hollow: u8::from(config.hollow),
            support_shape: match config.supports.shape {
                SupportShape::Cylinder => 0,
                SupportShape::Tapered => 1,
                SupportShape::Square => 2,
            },
            support_overhang_angle_deg: config.supports.overhang_angle_deg,
            support_spacing_mm: config.supports.spacing_mm,
            support_tip_diameter_mm: config.supports.tip_diameter_mm,
            support_trunk_diameter_mm: config.supports.trunk_diameter_mm,
            support_base_diameter_mm: config.supports.base_diameter_mm,
            support_tip_height_mm: config.supports.tip_height_mm,
            support_base_height_mm: config.supports.base_height_mm,
            support_penetration_mm: config.supports.penetration_mm,
            support_min_height_mm: config.supports.min_height_mm,
            support_max_height_mm: config.supports.max_height_mm,
            support_radial_segments: config.supports.radial_segments,
            xy_offset_mm: config.xy_offset_mm,
        }
    }
}

impl From<AnycubicSlicerConfig> for SlicerConfig {
    fn from(config: AnycubicSlicerConfig) -> Self {
        let mut result = Self::default();
        result.print.layer_height_mm = config.layer_height_mm;
        result.print.bottom_layers = config.bottom_layers;
        result.print.bottom_exposure_s = config.bottom_exposure_s;
        result.print.exposure_s = config.exposure_s;
        result.print.lift_height_mm = config.lift_height_mm;
        result.print.lift_speed_mm_s = config.lift_speed_mm_s;
        result.print.off_time_s = config.off_time_s;
        result.transform.translation_mm = config.translation_mm;
        result.transform.rotation_deg = config.rotation_deg;
        result.transform.scale = config.scale;
        result.transform.auto_center_xy = config.auto_center_xy != 0;
        result.transform.place_on_bed = config.place_on_bed != 0;
        result.supports.enabled = config.supports_enabled != 0;
        result.supports.auto_generate = config.supports_auto_generate != 0;
        result.supports.shape = match config.support_shape {
            0 => SupportShape::Cylinder,
            2 => SupportShape::Square,
            _ => SupportShape::Tapered,
        };
        result.supports.overhang_angle_deg = config.support_overhang_angle_deg;
        result.supports.spacing_mm = config.support_spacing_mm;
        result.supports.tip_diameter_mm = config.support_tip_diameter_mm;
        result.supports.trunk_diameter_mm = config.support_trunk_diameter_mm;
        result.supports.base_diameter_mm = config.support_base_diameter_mm;
        result.supports.tip_height_mm = config.support_tip_height_mm;
        result.supports.base_height_mm = config.support_base_height_mm;
        result.supports.penetration_mm = config.support_penetration_mm;
        result.supports.min_height_mm = config.support_min_height_mm;
        result.supports.max_height_mm = config.support_max_height_mm;
        result.supports.radial_segments = config.support_radial_segments;
        result.hollow = config.hollow != 0;
        result.xy_offset_mm = config.xy_offset_mm;
        result
    }
}

/// Writes the ABI defaults into `output`. Returns zero on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn anycubic_slicer_default_config(output: *mut AnycubicSlicerConfig) -> i32 {
    if output.is_null() {
        set_last_error("output configuration pointer is null");
        return -1;
    }
    // SAFETY: the caller supplied a non-null pointer and the C contract
    // requires it to reference writable AnycubicSlicerConfig storage.
    unsafe { output.write(AnycubicSlicerConfig::default()) };
    clear_last_error();
    0
}

/// Slices `input_path` to `output_path`. Both must be NUL-terminated UTF-8.
/// Returns zero on success; details for failures are available through
/// `anycubic_slicer_last_error` on the same thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn anycubic_slicer_slice(
    input_path: *const c_char,
    output_path: *const c_char,
    configuration: *const AnycubicSlicerConfig,
) -> i32 {
    // SAFETY: this forwards the same validated C pointers and an empty manual
    // support array to the extended entry point.
    unsafe {
        anycubic_slicer_slice_with_supports(
            input_path,
            output_path,
            configuration,
            std::ptr::null(),
            0,
        )
    }
}

/// Extended entry point accepting explicit manual support contacts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn anycubic_slicer_slice_with_supports(
    input_path: *const c_char,
    output_path: *const c_char,
    configuration: *const AnycubicSlicerConfig,
    support_points: *const AnycubicSupportPoint,
    support_point_count: usize,
) -> i32 {
    if input_path.is_null() || output_path.is_null() || configuration.is_null() {
        set_last_error("input, output and configuration pointers must not be null");
        return -1;
    }
    if support_point_count != 0 && support_points.is_null() {
        set_last_error("manual support pointer is null while its count is non-zero");
        return -1;
    }
    // SAFETY: pointers were checked above; validity and NUL termination are
    // part of the exported C function contract.
    let input = unsafe { CStr::from_ptr(input_path) };
    // SAFETY: same contract as for input_path.
    let output = unsafe { CStr::from_ptr(output_path) };
    let input = match input.to_str() {
        Ok(value) => PathBuf::from(value),
        Err(error) => {
            set_last_error(&format!("input path is not UTF-8: {error}"));
            return -1;
        }
    };
    let output = match output.to_str() {
        Ok(value) => PathBuf::from(value),
        Err(error) => {
            set_last_error(&format!("output path is not UTF-8: {error}"));
            return -1;
        }
    };
    // SAFETY: the non-null pointer must reference a properly initialized
    // AnycubicSlicerConfig according to the exported C contract.
    let raw_config = unsafe { configuration.read() };
    if raw_config.support_shape > 2 {
        set_last_error("support_shape must be 0 (cylinder), 1 (tapered), or 2 (square)");
        return -1;
    }
    let mut config = SlicerConfig::from(raw_config);
    if support_point_count != 0 {
        // SAFETY: the caller supplied a non-null array containing the declared
        // number of AnycubicSupportPoint records.
        let points = unsafe { std::slice::from_raw_parts(support_points, support_point_count) };
        config
            .supports
            .manual_points
            .extend(points.iter().map(|point| SupportPoint {
                contact_mm: point.contact_mm,
                base_z_mm: point.base_z_mm.is_finite().then_some(point.base_z_mm),
            }));
        config.supports.enabled = true;
    }
    match slice_file(input, output, &config) {
        Ok(_) => {
            clear_last_error();
            0
        }
        Err(error) => {
            set_last_error(&error.to_string());
            -2
        }
    }
}

/// Returns a thread-local NUL-terminated error string. It remains valid until
/// the next slicer ABI call on the same thread.
#[unsafe(no_mangle)]
pub extern "C" fn anycubic_slicer_last_error() -> *const c_char {
    LAST_ERROR.with(|error| error.borrow().as_ptr())
}

fn set_last_error(message: &str) {
    let sanitized = message.replace('\0', " ");
    LAST_ERROR.with(|error| {
        *error.borrow_mut() = CString::new(sanitized).expect("interior NUL bytes were removed")
    });
}

fn clear_last_error() {
    LAST_ERROR.with(|error| *error.borrow_mut() = CString::default());
}
