//! Stable C ABI for embedding the slicer in native applications.

use std::{
    cell::RefCell,
    ffi::{CStr, CString, c_char},
    path::PathBuf,
};

use super::{SlicerConfig, slice_file};

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
    pub hollow: u8,
    pub xy_offset_mm: f64,
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
            hollow: u8::from(config.hollow),
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
    if input_path.is_null() || output_path.is_null() || configuration.is_null() {
        set_last_error("input, output and configuration pointers must not be null");
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
    let config = SlicerConfig::from(unsafe { configuration.read() });
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
