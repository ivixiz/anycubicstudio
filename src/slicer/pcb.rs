//! KiCad Gerber/Excellon artwork to a single-layer M7 Pro exposure job.
//!
//! Artwork is composited at the printer's native LCD resolution before its
//! pixel boundary is converted back to the vector `pwszImg` representation.
//! This gives Gerber dark/clear polarity deterministic boolean semantics while
//! retaining the exact shared coordinate system used by KiCad drill files.

use std::{
    collections::{HashMap, HashSet},
    f64::consts::PI,
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};

use crate::{Error, Result};

use super::{
    MachineConfig, PrintConfig, SlicerConfig, SupportConfig, Transform,
    geometry::{LayerSlice, Vec2, layer_from_contours},
    mesh::Mesh,
    render::RenderedImage,
    viewer_scene::write_viewer_scene,
    writer::write_pwsz_with_previews,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum DrillMode {
    /// Drill diameters are removed from the exposed copper artwork. This leaves
    /// visible centring marks while preserving the annular part of a pad.
    Clear,
    /// Drill diameters are added to the exposed image.
    Expose,
    /// The Excellon file is parsed and aligned but does not affect the image.
    Ignore,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PcbExposureConfig {
    pub machine: MachineConfig,
    pub exposure_s: f64,
    pub layer_height_mm: f64,
    /// A zero lift avoids an intentional per-layer lift. Firmware may still
    /// perform its normal print-start homing sequence.
    pub lift_height_mm: f64,
    pub lift_speed_mm_s: f64,
    pub offset_mm: [f64; 2],
    pub rotation_deg: f64,
    pub scale: f64,
    /// Automatically mirror the complete Gerber+drill group on X when any
    /// Gerber filename identifies a KiCad front-side production layer.
    #[serde(default = "default_auto_mirror_front_layers")]
    pub auto_mirror_front_layers: bool,
    pub mirror_x: bool,
    pub mirror_y: bool,
    /// Inverts pixels inside the transformed artwork bounding rectangle only,
    /// never the complete LCD.
    pub invert: bool,
    pub invert_margin_mm: f64,
    pub drill_mode: DrillMode,
    /// Raster-boundary simplification. Zero retains every pixel step.
    pub simplify_tolerance_mm: f64,
}

impl Default for PcbExposureConfig {
    fn default() -> Self {
        Self {
            machine: MachineConfig::photon_mono_m7_pro(),
            exposure_s: 60.0,
            layer_height_mm: 0.05,
            lift_height_mm: 0.0,
            lift_speed_mm_s: 6.0,
            offset_mm: [0.0, 0.0],
            rotation_deg: 0.0,
            scale: 1.0,
            auto_mirror_front_layers: true,
            mirror_x: false,
            mirror_y: false,
            invert: false,
            invert_margin_mm: 0.0,
            drill_mode: DrillMode::Clear,
            simplify_tolerance_mm: 0.012,
        }
    }
}

const FRONT_LAYER_MARKERS: [&str; 4] = ["f_cu", "f_mask", "f_paste", "f_silkscreen"];

const fn default_auto_mirror_front_layers() -> bool {
    true
}

/// Returns true for conventional KiCad front-side production filenames such
/// as `board-F_Cu.gbr`, `board-F_Mask.gbr`, `board-F_Paste.gbr`, and
/// `board-F_Silkscreen.gbr`. Matching is case-insensitive and treats runs of
/// punctuation as one underscore, so `F.Cu` and `F-Cu` are accepted too.
pub fn filename_suggests_front_mirror(path: impl AsRef<Path>) -> bool {
    let Some(name) = path.as_ref().file_name().and_then(|name| name.to_str()) else {
        return false;
    };
    let mut normalized = String::with_capacity(name.len());
    for character in name.chars().flat_map(char::to_lowercase) {
        if character.is_ascii_alphanumeric() {
            normalized.push(character);
        } else if !normalized.ends_with('_') {
            normalized.push('_');
        }
    }
    let padded = format!("_{normalized}_");
    FRONT_LAYER_MARKERS
        .iter()
        .any(|marker| padded.contains(&format!("_{marker}_")))
}

#[derive(Debug, Clone)]
pub struct PcbExposureReport {
    pub gerber_files: usize,
    pub drill_files: usize,
    pub exposure_s: f64,
    pub exposed_pixels: u64,
    pub contour_count: usize,
    pub segment_count: usize,
    pub bounds_mm: [f64; 4],
    pub exposed_area_mm2: f64,
    pub output_bytes: u64,
    pub warnings: Vec<String>,
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
struct Point {
    x: f64,
    y: f64,
}

#[derive(Debug, Clone)]
enum Shape {
    Circle { center: Point, diameter: f64 },
    Stroke { points: Vec<Point>, width: f64 },
    Polygon(Vec<Point>),
}

#[derive(Debug, Clone)]
struct Operation {
    shape: Shape,
    dark: bool,
}

impl Shape {
    fn bounds(&self) -> Option<[f64; 4]> {
        match self {
            Self::Circle { center, diameter } => {
                let radius = diameter * 0.5;
                Some([
                    center.x - radius,
                    center.y - radius,
                    center.x + radius,
                    center.y + radius,
                ])
            }
            Self::Stroke { points, width } => points_bounds(points).map(|mut bounds| {
                let radius = width * 0.5;
                bounds[0] -= radius;
                bounds[1] -= radius;
                bounds[2] += radius;
                bounds[3] += radius;
                bounds
            }),
            Self::Polygon(points) => points_bounds(points),
        }
    }

    fn transformed(&self, transform: &ArtworkTransform) -> Self {
        match self {
            Self::Circle { center, diameter } => Self::Circle {
                center: transform.point(*center),
                diameter: diameter * transform.scale,
            },
            Self::Stroke { points, width } => Self::Stroke {
                points: points.iter().map(|point| transform.point(*point)).collect(),
                width: width * transform.scale,
            },
            Self::Polygon(points) => {
                Self::Polygon(points.iter().map(|point| transform.point(*point)).collect())
            }
        }
    }
}

#[derive(Debug, Clone)]
struct ParsedArtwork {
    operations: Vec<Operation>,
    negative: bool,
    warnings: Vec<String>,
}

#[derive(Debug, Clone, Copy)]
struct ArtworkTransform {
    source_center: Point,
    target_center: Point,
    radians: f64,
    scale: f64,
    mirror_x: bool,
    mirror_y: bool,
}

impl ArtworkTransform {
    fn point(self, point: Point) -> Point {
        let mut x = (point.x - self.source_center.x) * self.scale;
        let mut y = (point.y - self.source_center.y) * self.scale;
        if self.mirror_x {
            x = -x;
        }
        if self.mirror_y {
            y = -y;
        }
        Point {
            x: x * self.radians.cos() - y * self.radians.sin() + self.target_center.x,
            y: x * self.radians.sin() + y * self.radians.cos() + self.target_center.y,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ApertureShape {
    Circle,
    Rectangle,
    Obround,
    Polygon,
    Macro,
    Unknown,
}

#[derive(Debug, Clone)]
struct Aperture {
    shape: ApertureShape,
    x: f64,
    y: f64,
    vertices: usize,
    rotation_deg: f64,
    hole_diameter: Option<f64>,
    macro_name: String,
    modifiers: Vec<f64>,
    unit_mm: f64,
}

impl Default for Aperture {
    fn default() -> Self {
        Self {
            shape: ApertureShape::Unknown,
            x: 0.1,
            y: 0.1,
            vertices: 0,
            rotation_deg: 0.0,
            hole_diameter: None,
            macro_name: String::new(),
            modifiers: Vec::new(),
            unit_mm: 1.0,
        }
    }
}

#[derive(Debug, Clone, Copy)]
struct CoordinateFormat {
    integer: usize,
    decimals: usize,
    trailing_omission: bool,
}

impl Default for CoordinateFormat {
    fn default() -> Self {
        Self {
            integer: 4,
            decimals: 6,
            trailing_omission: false,
        }
    }
}

fn invalid(message: impl Into<String>) -> Error {
    Error::InvalidArtwork(message.into())
}

fn points_bounds(points: &[Point]) -> Option<[f64; 4]> {
    points.iter().copied().fold(None, |bounds, point| {
        Some(match bounds {
            None => [point.x, point.y, point.x, point.y],
            Some([min_x, min_y, max_x, max_y]) => [
                min_x.min(point.x),
                min_y.min(point.y),
                max_x.max(point.x),
                max_y.max(point.y),
            ],
        })
    })
}

fn operations_bounds(operations: &[Operation]) -> Option<[f64; 4]> {
    operations
        .iter()
        .filter_map(|operation| operation.shape.bounds())
        .fold(None, |combined: Option<[f64; 4]>, bounds| {
            Some(match combined {
                None => bounds,
                Some(current) => [
                    current[0].min(bounds[0]),
                    current[1].min(bounds[1]),
                    current[2].max(bounds[2]),
                    current[3].max(bounds[3]),
                ],
            })
        })
}

fn parse_number(text: &str) -> f64 {
    text.trim().parse().unwrap_or(0.0)
}

struct MacroExpression<'a> {
    text: &'a [u8],
    position: usize,
    variables: &'a [f64],
}

impl<'a> MacroExpression<'a> {
    fn evaluate(text: &'a str, variables: &'a [f64]) -> f64 {
        let mut parser = Self {
            text: text.as_bytes(),
            position: 0,
            variables,
        };
        parser.add_subtract()
    }

    fn skip_spaces(&mut self) {
        while self
            .text
            .get(self.position)
            .is_some_and(u8::is_ascii_whitespace)
        {
            self.position += 1;
        }
    }

    fn add_subtract(&mut self) -> f64 {
        let mut value = self.multiply_divide();
        loop {
            self.skip_spaces();
            let Some(operation @ (b'+' | b'-')) = self.text.get(self.position).copied() else {
                return value;
            };
            self.position += 1;
            let operand = self.multiply_divide();
            if operation == b'+' {
                value += operand;
            } else {
                value -= operand;
            }
        }
    }

    fn multiply_divide(&mut self) -> f64 {
        let mut value = self.unary();
        loop {
            self.skip_spaces();
            let Some(operation @ (b'x' | b'X' | b'/')) = self.text.get(self.position).copied()
            else {
                return value;
            };
            self.position += 1;
            let operand = self.unary();
            if operation == b'/' {
                if operand != 0.0 {
                    value /= operand;
                }
            } else {
                value *= operand;
            }
        }
    }

    fn unary(&mut self) -> f64 {
        self.skip_spaces();
        match self.text.get(self.position) {
            Some(b'+') => {
                self.position += 1;
                self.unary()
            }
            Some(b'-') => {
                self.position += 1;
                -self.unary()
            }
            _ => self.primary(),
        }
    }

    fn primary(&mut self) -> f64 {
        self.skip_spaces();
        if self.text.get(self.position) == Some(&b'(') {
            self.position += 1;
            let value = self.add_subtract();
            self.skip_spaces();
            if self.text.get(self.position) == Some(&b')') {
                self.position += 1;
            }
            return value;
        }
        if self.text.get(self.position) == Some(&b'$') {
            self.position += 1;
            let start = self.position;
            while self.text.get(self.position).is_some_and(u8::is_ascii_digit) {
                self.position += 1;
            }
            let index = std::str::from_utf8(&self.text[start..self.position])
                .ok()
                .and_then(|value| value.parse::<usize>().ok())
                .unwrap_or(0);
            return self.variables.get(index).copied().unwrap_or(0.0);
        }
        let start = self.position;
        while let Some(character) = self.text.get(self.position).copied() {
            if character.is_ascii_digit() || matches!(character, b'.' | b'e' | b'E') {
                self.position += 1;
                if matches!(character, b'e' | b'E')
                    && self
                        .text
                        .get(self.position)
                        .is_some_and(|next| matches!(next, b'+' | b'-'))
                {
                    self.position += 1;
                }
            } else {
                break;
            }
        }
        std::str::from_utf8(&self.text[start..self.position])
            .ok()
            .and_then(|value| value.parse().ok())
            .unwrap_or(0.0)
    }
}

fn rotated_point(point: Point, degrees: f64, center: Point) -> Point {
    let radians = degrees.to_radians();
    Point {
        x: center.x + point.x * radians.cos() - point.y * radians.sin(),
        y: center.y + point.x * radians.sin() + point.y * radians.cos(),
    }
}

fn regular_polygon(center: Point, diameter: f64, vertices: usize, rotation: f64) -> Shape {
    let radius = diameter * 0.5;
    Shape::Polygon(
        (0..vertices)
            .map(|index| {
                let angle = (rotation + 360.0 * index as f64 / vertices as f64).to_radians();
                Point {
                    x: center.x + radius * angle.cos(),
                    y: center.y + radius * angle.sin(),
                }
            })
            .collect(),
    )
}

fn rectangle(center: Point, width: f64, height: f64, rotation: f64) -> Shape {
    let points = [
        Point {
            x: -width * 0.5,
            y: -height * 0.5,
        },
        Point {
            x: width * 0.5,
            y: -height * 0.5,
        },
        Point {
            x: width * 0.5,
            y: height * 0.5,
        },
        Point {
            x: -width * 0.5,
            y: height * 0.5,
        },
    ];
    Shape::Polygon(
        points
            .into_iter()
            .map(|point| rotated_point(point, rotation, center))
            .collect(),
    )
}

fn extract_macros(source: &str) -> (String, HashMap<String, Vec<String>>) {
    let mut remainder = source.to_owned();
    let mut macros = HashMap::new();
    while let Some(start) = remainder.find("%AM") {
        let Some(relative_end) = remainder[start + 3..].find('%') else {
            break;
        };
        let end = start + 3 + relative_end;
        let body = remainder[start + 3..end].to_owned();
        let mut blocks = body.split('*');
        let name = blocks.next().unwrap_or_default().trim().to_owned();
        macros.insert(
            name,
            blocks
                .map(str::trim)
                .filter(|value| !value.is_empty())
                .map(str::to_owned)
                .collect(),
        );
        remainder.replace_range(start..=end, "");
    }
    (remainder, macros)
}

fn coordinate_fields(command: &str) -> HashMap<char, String> {
    let bytes = command.as_bytes();
    let mut fields = HashMap::new();
    let mut position = 0;
    while position < bytes.len() {
        let letter = bytes[position] as char;
        if !matches!(letter, 'X' | 'Y' | 'I' | 'J') {
            position += 1;
            continue;
        }
        let start = position + 1;
        let mut end = start;
        if bytes
            .get(end)
            .is_some_and(|value| matches!(value, b'+' | b'-'))
        {
            end += 1;
        }
        while bytes
            .get(end)
            .is_some_and(|value| value.is_ascii_digit() || *value == b'.')
        {
            end += 1;
        }
        if end > start {
            fields.insert(letter, command[start..end].to_owned());
            position = end;
        } else {
            position += 1;
        }
    }
    fields
}

fn d_code(command: &str) -> Option<u32> {
    let bytes = command.as_bytes();
    let mut result = None;
    let mut position = 0;
    while position < bytes.len() {
        if bytes[position] != b'D' {
            position += 1;
            continue;
        }
        let start = position + 1;
        let mut end = start;
        while bytes.get(end).is_some_and(u8::is_ascii_digit) {
            end += 1;
        }
        if end > start {
            result = command[start..end].parse().ok();
            position = end;
        } else {
            position += 1;
        }
    }
    result
}

fn parse_coordinate(text: &str, format: CoordinateFormat, unit_mm: f64) -> f64 {
    if text.contains('.') {
        return parse_number(text) * unit_mm;
    }
    let negative = text.starts_with('-');
    let digits = text.trim_start_matches(['+', '-']);
    let mut value = digits.parse::<f64>().unwrap_or(0.0);
    if format.trailing_omission {
        let total = format.integer + format.decimals;
        if digits.len() < total {
            value *= 10_f64.powi((total - digits.len()) as i32);
        }
    }
    value /= 10_f64.powi(format.decimals as i32);
    if negative {
        -value * unit_mm
    } else {
        value * unit_mm
    }
}

fn parse_format(command: &str, x: &mut CoordinateFormat, y: &mut CoordinateFormat) {
    let trailing = command.get(2..3) == Some("T");
    for (letter, target) in [('X', x), ('Y', y)] {
        let Some(position) = command.find(letter) else {
            continue;
        };
        let digits = command[position + 1..]
            .bytes()
            .take_while(u8::is_ascii_digit)
            .collect::<Vec<_>>();
        if digits.len() >= 2 {
            target.integer = usize::from(digits[0] - b'0');
            target.decimals = usize::from(digits[1] - b'0');
            target.trailing_omission = trailing;
        }
    }
}

fn parse_aperture(
    command: &str,
    unit_mm: f64,
    macros: &HashMap<String, Vec<String>>,
) -> Option<(u32, Aperture)> {
    let rest = command.strip_prefix("ADD")?;
    let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
    let code = rest[..digits].parse().ok()?;
    let definition = &rest[digits..];
    let (template, modifiers) = definition
        .split_once(',')
        .map_or((definition, ""), |(template, modifiers)| {
            (template, modifiers)
        });
    let raw_modifiers = modifiers
        .split(['X', 'x'])
        .filter(|value| !value.trim().is_empty())
        .map(parse_number)
        .collect::<Vec<_>>();
    let mut aperture = Aperture {
        unit_mm,
        modifiers: raw_modifiers.clone(),
        ..Aperture::default()
    };
    aperture.shape = match template {
        "C" => ApertureShape::Circle,
        "R" => ApertureShape::Rectangle,
        "O" => ApertureShape::Obround,
        "P" => ApertureShape::Polygon,
        name if macros.contains_key(name) => {
            aperture.macro_name = name.to_owned();
            ApertureShape::Macro
        }
        _ => ApertureShape::Unknown,
    };
    aperture.x = raw_modifiers.first().copied().unwrap_or(0.1) * unit_mm;
    match aperture.shape {
        ApertureShape::Circle => {
            aperture.y = aperture.x;
            aperture.hole_diameter = raw_modifiers.get(1).map(|value| value * unit_mm);
        }
        ApertureShape::Rectangle | ApertureShape::Obround => {
            aperture.y = raw_modifiers
                .get(1)
                .copied()
                .unwrap_or(raw_modifiers.first().copied().unwrap_or(0.1))
                * unit_mm;
            aperture.hole_diameter = raw_modifiers.get(2).map(|value| value * unit_mm);
        }
        ApertureShape::Polygon => {
            aperture.y = aperture.x;
            aperture.vertices = raw_modifiers
                .get(1)
                .copied()
                .unwrap_or(0.0)
                .round()
                .max(0.0) as usize;
            aperture.rotation_deg = raw_modifiers.get(2).copied().unwrap_or(0.0);
            aperture.hole_diameter = raw_modifiers.get(3).map(|value| value * unit_mm);
        }
        ApertureShape::Macro | ApertureShape::Unknown => {}
    }
    Some((code, aperture))
}

fn flash_aperture(
    aperture: &Aperture,
    center: Point,
    dark: bool,
    macros: &HashMap<String, Vec<String>>,
    warnings: &mut Vec<String>,
) -> Vec<Operation> {
    let mut operations = Vec::new();
    let shape = match aperture.shape {
        ApertureShape::Circle => Some(Shape::Circle {
            center,
            diameter: aperture.x,
        }),
        ApertureShape::Rectangle => Some(rectangle(center, aperture.x, aperture.y, 0.0)),
        ApertureShape::Obround => {
            let diameter = aperture.x.min(aperture.y);
            let mut first = center;
            let mut second = center;
            if aperture.x >= aperture.y {
                let offset = (aperture.x - aperture.y) * 0.5;
                first.x -= offset;
                second.x += offset;
            } else {
                let offset = (aperture.y - aperture.x) * 0.5;
                first.y -= offset;
                second.y += offset;
            }
            Some(Shape::Stroke {
                points: vec![first, second],
                width: diameter,
            })
        }
        ApertureShape::Polygon if aperture.vertices >= 3 => Some(regular_polygon(
            center,
            aperture.x,
            aperture.vertices,
            aperture.rotation_deg,
        )),
        ApertureShape::Macro => {
            operations.extend(macro_flash(aperture, center, dark, macros, warnings));
            None
        }
        _ => {
            push_warning(
                warnings,
                format!("unsupported aperture '{}'", aperture.macro_name),
            );
            None
        }
    };
    if let Some(shape) = shape {
        operations.push(Operation { shape, dark });
        if let Some(diameter) = aperture.hole_diameter.filter(|diameter| *diameter > 0.0) {
            operations.push(Operation {
                shape: Shape::Circle { center, diameter },
                dark: !dark,
            });
        }
    }
    operations
}

fn macro_flash(
    aperture: &Aperture,
    center: Point,
    dark: bool,
    macros: &HashMap<String, Vec<String>>,
    warnings: &mut Vec<String>,
) -> Vec<Operation> {
    let mut variables = vec![0.0; aperture.modifiers.len() + 1];
    for (index, value) in aperture.modifiers.iter().enumerate() {
        variables[index + 1] = *value;
    }
    let mut operations = Vec::new();
    let value =
        |expression: &str, variables: &[f64]| MacroExpression::evaluate(expression, variables);
    for block in macros.get(&aperture.macro_name).into_iter().flatten() {
        let block = block.trim();
        if block.is_empty() || block.starts_with('0') {
            continue;
        }
        if let Some(assignment) = block.strip_prefix('$') {
            if let Some((index, expression)) = assignment.split_once('=')
                && let Ok(index) = index.parse::<usize>()
            {
                if variables.len() <= index {
                    variables.resize(index + 1, 0.0);
                }
                variables[index] = value(expression, &variables);
            }
            continue;
        }
        let fields = block.split(',').map(str::trim).collect::<Vec<_>>();
        if fields.len() < 2 {
            continue;
        }
        let primitive = fields[0].parse::<u32>().unwrap_or(0);
        let exposed = value(fields[1], &variables) != 0.0;
        let primitive_dark = if exposed { dark } else { !dark };
        let unit = aperture.unit_mm;
        let operation = match primitive {
            1 if fields.len() >= 5 => {
                let diameter = value(fields[2], &variables) * unit;
                let rotation = fields.get(5).map_or(0.0, |field| value(field, &variables));
                let offset = rotated_point(
                    Point {
                        x: value(fields[3], &variables) * unit,
                        y: value(fields[4], &variables) * unit,
                    },
                    rotation,
                    center,
                );
                Some(Operation {
                    shape: Shape::Circle {
                        center: offset,
                        diameter,
                    },
                    dark: primitive_dark,
                })
            }
            4 if fields.len() >= 8 => {
                let vertices = value(fields[2], &variables).round().max(0.0) as usize;
                let rotation_index = 3 + (vertices + 1) * 2;
                if fields.len() <= rotation_index {
                    None
                } else {
                    let rotation = value(fields[rotation_index], &variables);
                    let mut points = Vec::new();
                    for vertex in 0..=vertices {
                        points.push(rotated_point(
                            Point {
                                x: value(fields[3 + vertex * 2], &variables) * unit,
                                y: value(fields[4 + vertex * 2], &variables) * unit,
                            },
                            rotation,
                            center,
                        ));
                    }
                    if points.first() == points.last() {
                        points.pop();
                    }
                    (points.len() >= 3).then_some(Operation {
                        shape: Shape::Polygon(points),
                        dark: primitive_dark,
                    })
                }
            }
            5 if fields.len() >= 7 => {
                let vertices = value(fields[2], &variables).round().max(0.0) as usize;
                let primitive_center = Point {
                    x: center.x + value(fields[3], &variables) * unit,
                    y: center.y + value(fields[4], &variables) * unit,
                };
                (vertices >= 3).then_some(Operation {
                    shape: regular_polygon(
                        primitive_center,
                        value(fields[5], &variables) * unit,
                        vertices,
                        value(fields[6], &variables),
                    ),
                    dark: primitive_dark,
                })
            }
            20 if fields.len() >= 8 => {
                let rotation = value(fields[7], &variables);
                let start = rotated_point(
                    Point {
                        x: value(fields[3], &variables) * unit,
                        y: value(fields[4], &variables) * unit,
                    },
                    rotation,
                    center,
                );
                let end = rotated_point(
                    Point {
                        x: value(fields[5], &variables) * unit,
                        y: value(fields[6], &variables) * unit,
                    },
                    rotation,
                    center,
                );
                Some(Operation {
                    shape: Shape::Stroke {
                        points: vec![start, end],
                        width: value(fields[2], &variables) * unit,
                    },
                    dark: primitive_dark,
                })
            }
            21 if fields.len() >= 7 => {
                let primitive_center = Point {
                    x: center.x + value(fields[4], &variables) * unit,
                    y: center.y + value(fields[5], &variables) * unit,
                };
                Some(Operation {
                    shape: rectangle(
                        primitive_center,
                        value(fields[2], &variables) * unit,
                        value(fields[3], &variables) * unit,
                        value(fields[6], &variables),
                    ),
                    dark: primitive_dark,
                })
            }
            _ => {
                push_warning(
                    warnings,
                    format!(
                        "unsupported aperture macro primitive {primitive} in '{}'",
                        aperture.macro_name
                    ),
                );
                None
            }
        };
        if let Some(operation) = operation {
            operations.push(operation);
        }
    }
    operations
}

fn push_warning(warnings: &mut Vec<String>, warning: String) {
    if !warnings.contains(&warning) {
        warnings.push(warning);
    }
}

fn arc_points(from: Point, to: Point, offset: Point, clockwise: bool) -> Vec<Point> {
    let center = Point {
        x: from.x + offset.x,
        y: from.y + offset.y,
    };
    let radius = ((from.x - center.x).powi(2) + (from.y - center.y).powi(2)).sqrt();
    if radius <= 1.0e-12 {
        return vec![from, to];
    }
    let start = (from.y - center.y).atan2(from.x - center.x);
    let end = (to.y - center.y).atan2(to.x - center.x);
    let mut sweep = end - start;
    if clockwise && sweep >= 0.0 {
        sweep -= 2.0 * PI;
    }
    if !clockwise && sweep <= 0.0 {
        sweep += 2.0 * PI;
    }
    let segments = ((sweep.abs() * radius / 0.02).ceil() as usize).clamp(8, 8192);
    (0..=segments)
        .map(|index| {
            let angle = start + sweep * index as f64 / segments as f64;
            Point {
                x: center.x + radius * angle.cos(),
                y: center.y + radius * angle.sin(),
            }
        })
        .collect()
}

fn parse_gerber(path: &Path) -> Result<ParsedArtwork> {
    let source = std::fs::read_to_string(path)?;
    let (source, macros) = extract_macros(&source.replace('\r', ""));
    let source = source.replace(['\n', '%'], " ");
    let mut apertures = HashMap::<u32, Aperture>::new();
    let mut x_format = CoordinateFormat::default();
    let mut y_format = CoordinateFormat::default();
    let mut unit_mm = 1.0;
    let mut active_aperture = 0;
    let mut operation = 2;
    let mut interpolation = 1;
    let mut absolute = true;
    let mut current = Point::default();
    let mut have_current = false;
    let mut dark = true;
    let mut negative = false;
    let mut in_region = false;
    let mut region = Vec::new();
    let mut operations = Vec::new();
    let mut warnings = Vec::new();

    for raw_command in source.split('*') {
        let command = raw_command.trim();
        if command.is_empty()
            || command.starts_with("G04")
            || command.starts_with("TF")
            || command.starts_with("TA")
            || command.starts_with("TO")
            || command.starts_with("TD")
        {
            continue;
        }
        if command.starts_with("FS") {
            parse_format(command, &mut x_format, &mut y_format);
            absolute = command.get(3..4).is_none_or(|mode| mode != "I");
            continue;
        }
        if command.starts_with("MOIN") {
            unit_mm = 25.4;
            continue;
        }
        if command.starts_with("MOMM") {
            unit_mm = 1.0;
            continue;
        }
        if let Some((code, aperture)) = parse_aperture(command, unit_mm, &macros) {
            if aperture.shape == ApertureShape::Unknown {
                push_warning(
                    &mut warnings,
                    format!("unsupported aperture definition D{code}: {command}"),
                );
            }
            apertures.insert(code, aperture);
            continue;
        }
        if command.starts_with("LPD") {
            dark = true;
            continue;
        }
        if command.starts_with("LPC") {
            dark = false;
            continue;
        }
        if command.starts_with("IPNEG") {
            negative = true;
            continue;
        }
        if command.starts_with("IPPOS") {
            negative = false;
            continue;
        }
        if command.contains("G90") {
            absolute = true;
        }
        if command.contains("G91") {
            absolute = false;
        }
        if command.contains("G36") {
            in_region = true;
            region.clear();
            continue;
        }
        if command.contains("G37") {
            if region.len() >= 3 {
                operations.push(Operation {
                    shape: Shape::Polygon(std::mem::take(&mut region)),
                    dark,
                });
            }
            in_region = false;
            continue;
        }
        if command.contains("G01") {
            interpolation = 1;
        }
        if command.contains("G02") {
            interpolation = 2;
        }
        if command.contains("G03") {
            interpolation = 3;
        }

        let values = coordinate_fields(command);
        if let Some(code) = d_code(command) {
            if code >= 10 && values.is_empty() {
                active_aperture = code;
                continue;
            }
            if code <= 3 {
                operation = code;
            }
        }
        if !values.contains_key(&'X') && !values.contains_key(&'Y') {
            continue;
        }
        let mut next = current;
        if let Some(value) = values.get(&'X') {
            let parsed = parse_coordinate(value, x_format, unit_mm);
            next.x = if absolute { parsed } else { current.x + parsed };
        }
        if let Some(value) = values.get(&'Y') {
            let parsed = parse_coordinate(value, y_format, unit_mm);
            next.y = if absolute { parsed } else { current.y + parsed };
        }

        if operation == 1 && have_current {
            let points = if interpolation == 1 {
                vec![current, next]
            } else {
                let offset = Point {
                    x: values
                        .get(&'I')
                        .map_or(0.0, |value| parse_coordinate(value, x_format, unit_mm)),
                    y: values
                        .get(&'J')
                        .map_or(0.0, |value| parse_coordinate(value, y_format, unit_mm)),
                };
                arc_points(current, next, offset, interpolation == 2)
            };
            if in_region {
                if region.is_empty() {
                    region.push(current);
                }
                region.extend(points.into_iter().skip(1));
            } else if let Some(aperture) = apertures.get(&active_aperture) {
                if aperture.shape == ApertureShape::Circle && aperture.x > 0.0 {
                    operations.push(Operation {
                        shape: Shape::Stroke {
                            points,
                            width: aperture.x,
                        },
                        dark,
                    });
                } else {
                    push_warning(
                        &mut warnings,
                        format!(
                            "D01 draw with non-circular aperture D{active_aperture} was skipped"
                        ),
                    );
                }
            } else {
                push_warning(
                    &mut warnings,
                    format!("undefined aperture D{active_aperture} was skipped"),
                );
            }
        } else if operation == 2 {
            if in_region {
                if region.len() >= 3 {
                    operations.push(Operation {
                        shape: Shape::Polygon(std::mem::take(&mut region)),
                        dark,
                    });
                }
                region.push(next);
            }
        } else if operation == 3 {
            if let Some(aperture) = apertures.get(&active_aperture) {
                operations.extend(flash_aperture(aperture, next, dark, &macros, &mut warnings));
            } else {
                push_warning(
                    &mut warnings,
                    format!("undefined aperture D{active_aperture} was skipped"),
                );
            }
        }
        current = next;
        have_current = true;
    }

    if operations.is_empty() {
        return Err(invalid(format!(
            "{} contains no supported Gerber geometry",
            path.display()
        )));
    }
    Ok(ParsedArtwork {
        operations,
        negative,
        warnings,
    })
}

fn parse_excellon(path: &Path) -> Result<ParsedArtwork> {
    let source = std::fs::read_to_string(path)?;
    let mut tools = HashMap::<u32, f64>::new();
    let mut active_tool = 0;
    let mut format = CoordinateFormat {
        integer: 3,
        decimals: 3,
        trailing_omission: false,
    };
    let mut unit_mm = 1.0;
    let mut current = Point::default();
    let mut have_current = false;
    let mut routing = false;
    let mut operations = Vec::new();
    let mut warnings = Vec::new();

    for raw_line in source.lines() {
        let line = raw_line.trim().to_ascii_uppercase();
        if line.is_empty() || line.starts_with(';') {
            continue;
        }
        if line.contains("METRIC") {
            unit_mm = 1.0;
            if line.contains("000.000") {
                format = CoordinateFormat {
                    integer: 3,
                    decimals: 3,
                    trailing_omission: false,
                };
            }
            continue;
        }
        if line.contains("INCH") {
            unit_mm = 25.4;
            format = CoordinateFormat {
                integer: 2,
                decimals: 4,
                trailing_omission: false,
            };
            continue;
        }
        if let Some(rest) = line.strip_prefix('T') {
            let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
            if digits > 0 {
                let tool = rest[..digits].parse::<u32>().unwrap_or(0);
                if let Some(c_position) = rest.find('C') {
                    let diameter = rest[c_position + 1..]
                        .bytes()
                        .take_while(|value| value.is_ascii_digit() || *value == b'.')
                        .count();
                    tools.insert(
                        tool,
                        parse_number(&rest[c_position + 1..c_position + 1 + diameter]) * unit_mm,
                    );
                } else if rest.len() == digits {
                    active_tool = tool;
                }
                continue;
            }
        }
        if line.contains("G00") {
            routing = false;
        }
        if line.contains("G01") {
            routing = true;
        }
        let has_coordinates = line.contains('X') || line.contains('Y');
        if !has_coordinates && !line.contains("G85") {
            continue;
        }
        let diameter = tools.get(&active_tool).copied().unwrap_or_else(|| {
            push_warning(
                &mut warnings,
                format!("undefined Excellon tool T{active_tool}; using 0.8 mm"),
            );
            0.8
        });
        if let Some(slot_position) = line.find("G85") {
            let first_values = coordinate_fields(&line[..slot_position]);
            let second_values = coordinate_fields(&line[slot_position + 3..]);
            let start = excellon_point(&first_values, current, format, unit_mm);
            let end = excellon_point(&second_values, start, format, unit_mm);
            operations.push(Operation {
                shape: Shape::Stroke {
                    points: vec![start, end],
                    width: diameter,
                },
                dark: true,
            });
            current = end;
            have_current = true;
            continue;
        }
        let values = coordinate_fields(&line);
        if !values.contains_key(&'X') && !values.contains_key(&'Y') {
            continue;
        }
        let next = excellon_point(&values, current, format, unit_mm);
        if routing && have_current {
            operations.push(Operation {
                shape: Shape::Stroke {
                    points: vec![current, next],
                    width: diameter,
                },
                dark: true,
            });
        } else if !line.contains("G00") {
            operations.push(Operation {
                shape: Shape::Circle {
                    center: next,
                    diameter,
                },
                dark: true,
            });
        }
        current = next;
        have_current = true;
    }
    if operations.is_empty() {
        push_warning(
            &mut warnings,
            format!("{} contains no drill coordinates; ignored", path.display()),
        );
    }
    Ok(ParsedArtwork {
        operations,
        negative: false,
        warnings,
    })
}

fn excellon_point(
    values: &HashMap<char, String>,
    current: Point,
    format: CoordinateFormat,
    unit_mm: f64,
) -> Point {
    Point {
        x: values
            .get(&'X')
            .map_or(current.x, |value| parse_coordinate(value, format, unit_mm)),
        y: values
            .get(&'Y')
            .map_or(current.y, |value| parse_coordinate(value, format, unit_mm)),
    }
}

#[derive(Debug, Clone, Copy)]
struct PixelRect {
    min_x: u32,
    min_y: u32,
    max_x: u32,
    max_y: u32,
}

impl PixelRect {
    fn union(self, other: Self) -> Self {
        Self {
            min_x: self.min_x.min(other.min_x),
            min_y: self.min_y.min(other.min_y),
            max_x: self.max_x.max(other.max_x),
            max_y: self.max_y.max(other.max_y),
        }
    }
}

struct BitGrid {
    width: u32,
    height: u32,
    physical_width: f64,
    physical_height: f64,
    bits: Vec<u8>,
}

impl BitGrid {
    fn new(machine: &MachineConfig) -> Result<Self> {
        let [width, height] = machine.resolution;
        if width == 0 || height == 0 {
            return Err(invalid("LCD resolution must be positive"));
        }
        let pixels = usize::try_from(u64::from(width) * u64::from(height))
            .map_err(|_| invalid("LCD bitmap is too large for this platform"))?;
        Ok(Self {
            width,
            height,
            physical_width: machine.build_volume_mm[0],
            physical_height: machine.build_volume_mm[1],
            bits: vec![0; pixels.div_ceil(8)],
        })
    }

    fn get(&self, x: u32, y: u32) -> bool {
        if x >= self.width || y >= self.height {
            return false;
        }
        let index = (u64::from(y) * u64::from(self.width) + u64::from(x)) as usize;
        self.bits[index / 8] & (1 << (index % 8)) != 0
    }

    fn set(&mut self, x: u32, y: u32, value: bool) {
        if x >= self.width || y >= self.height {
            return;
        }
        let index = (u64::from(y) * u64::from(self.width) + u64::from(x)) as usize;
        let mask = 1 << (index % 8);
        if value {
            self.bits[index / 8] |= mask;
        } else {
            self.bits[index / 8] &= !mask;
        }
    }

    fn or_assign(&mut self, other: &Self) {
        for (target, source) in self.bits.iter_mut().zip(&other.bits) {
            *target |= *source;
        }
    }

    fn pixel_width(&self) -> f64 {
        self.physical_width / f64::from(self.width)
    }

    fn pixel_height(&self) -> f64 {
        self.physical_height / f64::from(self.height)
    }

    fn pixel_center(&self, x: u32, y: u32) -> Point {
        Point {
            x: -self.physical_width * 0.5 + (f64::from(x) + 0.5) * self.pixel_width(),
            y: -self.physical_height * 0.5 + (f64::from(y) + 0.5) * self.pixel_height(),
        }
    }

    fn vertex(&self, x: u32, y: u32) -> Point {
        Point {
            x: -self.physical_width * 0.5 + f64::from(x) * self.pixel_width(),
            y: -self.physical_height * 0.5 + f64::from(y) * self.pixel_height(),
        }
    }

    fn rect_for_bounds(&self, bounds: [f64; 4]) -> Option<PixelRect> {
        if bounds[2] < -self.physical_width * 0.5
            || bounds[0] > self.physical_width * 0.5
            || bounds[3] < -self.physical_height * 0.5
            || bounds[1] > self.physical_height * 0.5
        {
            return None;
        }
        let x = |value: f64| {
            ((value + self.physical_width * 0.5) / self.pixel_width())
                .floor()
                .clamp(0.0, f64::from(self.width - 1)) as u32
        };
        let y = |value: f64| {
            ((value + self.physical_height * 0.5) / self.pixel_height())
                .floor()
                .clamp(0.0, f64::from(self.height - 1)) as u32
        };
        Some(PixelRect {
            min_x: x(bounds[0]),
            min_y: y(bounds[1]),
            max_x: x(bounds[2]),
            max_y: y(bounds[3]),
        })
    }

    fn count(&self) -> u64 {
        self.bits
            .iter()
            .map(|byte| u64::from(byte.count_ones()))
            .sum()
    }
}

fn raster_operation(grid: &mut BitGrid, operation: &Operation) -> Option<PixelRect> {
    let bounds = operation.shape.bounds()?;
    let rect = grid.rect_for_bounds(bounds)?;
    match &operation.shape {
        Shape::Circle { center, diameter } => {
            let radius_squared = (diameter * 0.5).powi(2);
            for y in rect.min_y..=rect.max_y {
                for x in rect.min_x..=rect.max_x {
                    let point = grid.pixel_center(x, y);
                    if (point.x - center.x).powi(2) + (point.y - center.y).powi(2) <= radius_squared
                    {
                        grid.set(x, y, operation.dark);
                    }
                }
            }
        }
        Shape::Stroke { points, width } => {
            let radius_squared = (width * 0.5).powi(2);
            for pair in points.windows(2) {
                let segment_bounds = Shape::Stroke {
                    points: pair.to_vec(),
                    width: *width,
                }
                .bounds()
                .and_then(|bounds| grid.rect_for_bounds(bounds));
                let Some(segment_rect) = segment_bounds else {
                    continue;
                };
                for y in segment_rect.min_y..=segment_rect.max_y {
                    for x in segment_rect.min_x..=segment_rect.max_x {
                        if point_segment_distance_squared(grid.pixel_center(x, y), pair[0], pair[1])
                            <= radius_squared
                        {
                            grid.set(x, y, operation.dark);
                        }
                    }
                }
            }
            if points.len() == 1 {
                raster_operation(
                    grid,
                    &Operation {
                        shape: Shape::Circle {
                            center: points[0],
                            diameter: *width,
                        },
                        dark: operation.dark,
                    },
                );
            }
        }
        Shape::Polygon(points) => {
            if points.len() < 3 {
                return Some(rect);
            }
            for y in rect.min_y..=rect.max_y {
                let scan_y = grid.pixel_center(0, y).y;
                let mut intersections = Vec::new();
                for index in 0..points.len() {
                    let first = points[index];
                    let second = points[(index + 1) % points.len()];
                    if (first.y > scan_y) != (second.y > scan_y) {
                        intersections.push(
                            first.x
                                + (second.x - first.x) * (scan_y - first.y) / (second.y - first.y),
                        );
                    }
                }
                intersections.sort_by(f64::total_cmp);
                for pair in intersections.chunks_exact(2) {
                    let span = grid.rect_for_bounds([pair[0], scan_y, pair[1], scan_y]);
                    if let Some(span) = span {
                        for x in span.min_x..=span.max_x {
                            let center_x = grid.pixel_center(x, y).x;
                            if center_x >= pair[0] && center_x <= pair[1] {
                                grid.set(x, y, operation.dark);
                            }
                        }
                    }
                }
            }
        }
    }
    Some(rect)
}

fn point_segment_distance_squared(point: Point, first: Point, second: Point) -> f64 {
    let dx = second.x - first.x;
    let dy = second.y - first.y;
    if dx == 0.0 && dy == 0.0 {
        return (point.x - first.x).powi(2) + (point.y - first.y).powi(2);
    }
    let ratio = ((point.x - first.x) * dx + (point.y - first.y) * dy) / (dx * dx + dy * dy);
    let ratio = ratio.clamp(0.0, 1.0);
    let nearest = Point {
        x: first.x + ratio * dx,
        y: first.y + ratio * dy,
    };
    (point.x - nearest.x).powi(2) + (point.y - nearest.y).powi(2)
}

fn invert_rect(grid: &mut BitGrid, bounds: [f64; 4]) -> Option<PixelRect> {
    let rect = grid.rect_for_bounds(bounds)?;
    for y in rect.min_y..=rect.max_y {
        for x in rect.min_x..=rect.max_x {
            let point = grid.pixel_center(x, y);
            if point.x >= bounds[0]
                && point.x <= bounds[2]
                && point.y >= bounds[1]
                && point.y <= bounds[3]
            {
                let value = !grid.get(x, y);
                grid.set(x, y, value);
            }
        }
    }
    Some(rect)
}

type Vertex = (u32, u32);

fn bitmap_contours(grid: &BitGrid, rect: PixelRect, tolerance: f64) -> Vec<Vec<Vec2>> {
    let mut outgoing = HashMap::<Vertex, Vec<Vertex>>::new();
    {
        let mut add = |start: Vertex, end: Vertex| {
            outgoing.entry(start).or_default().push(end);
        };
        for y in rect.min_y..=rect.max_y {
            for x in rect.min_x..=rect.max_x {
                if !grid.get(x, y) {
                    continue;
                }
                if y == 0 || !grid.get(x, y - 1) {
                    add((x, y), (x + 1, y));
                }
                if x + 1 >= grid.width || !grid.get(x + 1, y) {
                    add((x + 1, y), (x + 1, y + 1));
                }
                if y + 1 >= grid.height || !grid.get(x, y + 1) {
                    add((x + 1, y + 1), (x, y + 1));
                }
                if x == 0 || !grid.get(x - 1, y) {
                    add((x, y + 1), (x, y));
                }
            }
        }
    }

    let mut contours = Vec::new();
    // A stable contour start is important because closed-loop RDP depends on
    // that start. HashMap iteration would otherwise make identical inputs
    // produce slightly different PWSZ segment streams between processes.
    while let Some(start) = outgoing.keys().min().copied() {
        let Some(first) = pop_edge(&mut outgoing, start, None) else {
            outgoing.remove(&start);
            continue;
        };
        let mut vertices = vec![start];
        let mut previous = start;
        let mut current = first;
        let mut guard = 0_usize;
        while current != start {
            vertices.push(current);
            let Some(next) = pop_edge(&mut outgoing, current, Some(previous)) else {
                vertices.clear();
                break;
            };
            previous = current;
            current = next;
            guard += 1;
            if guard > 2_000_000 {
                vertices.clear();
                break;
            }
        }
        if vertices.len() < 3 {
            continue;
        }
        remove_collinear_vertices(&mut vertices);
        let mut points = vertices
            .into_iter()
            .map(|(x, y)| {
                let point = grid.vertex(x, y);
                Vec2 {
                    x: point.x,
                    y: point.y,
                }
            })
            .collect::<Vec<_>>();
        if tolerance > 0.0 && points.len() > 4 {
            points = simplify_closed(&points, tolerance);
        }
        if points.len() >= 3 && polygon_twice_area(&points).abs() > 1.0e-10 {
            contours.push(points);
        }
    }
    contours
}

fn pop_edge(
    outgoing: &mut HashMap<Vertex, Vec<Vertex>>,
    start: Vertex,
    previous: Option<Vertex>,
) -> Option<Vertex> {
    let (end, empty) = {
        let edges = outgoing.get_mut(&start)?;
        let index = if let Some(previous) = previous {
            let incoming = (
                i64::from(start.0) - i64::from(previous.0),
                i64::from(start.1) - i64::from(previous.1),
            );
            edges
                .iter()
                .enumerate()
                .max_by_key(|(_, end)| {
                    let outgoing = (
                        i64::from(end.0) - i64::from(start.0),
                        i64::from(end.1) - i64::from(start.1),
                    );
                    let cross = incoming.0 * outgoing.1 - incoming.1 * outgoing.0;
                    let dot = incoming.0 * outgoing.0 + incoming.1 * outgoing.1;
                    (cross, dot)
                })
                .map(|(index, _)| index)
                .unwrap_or(edges.len() - 1)
        } else {
            edges.len() - 1
        };
        let end = edges.swap_remove(index);
        (end, edges.is_empty())
    };
    if empty {
        outgoing.remove(&start);
    }
    Some(end)
}

fn remove_collinear_vertices(vertices: &mut Vec<Vertex>) {
    loop {
        if vertices.len() < 4 {
            return;
        }
        let mut removed = false;
        for index in 0..vertices.len() {
            let previous = vertices[(index + vertices.len() - 1) % vertices.len()];
            let current = vertices[index];
            let next = vertices[(index + 1) % vertices.len()];
            if (previous.0 == current.0 && current.0 == next.0)
                || (previous.1 == current.1 && current.1 == next.1)
            {
                vertices.remove(index);
                removed = true;
                break;
            }
        }
        if !removed {
            return;
        }
    }
}

fn simplify_closed(points: &[Vec2], tolerance: f64) -> Vec<Vec2> {
    let first = points[0];
    let split = points
        .iter()
        .enumerate()
        .max_by(|(_, left), (_, right)| {
            distance_squared(**left, first).total_cmp(&distance_squared(**right, first))
        })
        .map(|(index, _)| index)
        .unwrap_or(points.len() / 2);
    if split == 0 || split + 1 >= points.len() {
        return points.to_vec();
    }
    let mut first_half = rdp(&points[..=split], tolerance);
    let mut wrapped = points[split..].to_vec();
    wrapped.push(first);
    let mut second_half = rdp(&wrapped, tolerance);
    first_half.pop();
    second_half.pop();
    first_half.extend(second_half);
    first_half
}

fn rdp(points: &[Vec2], tolerance: f64) -> Vec<Vec2> {
    if points.len() <= 2 {
        return points.to_vec();
    }
    let mut maximum = 0.0;
    let mut split = 0;
    for (index, point) in points[1..points.len() - 1].iter().enumerate() {
        let distance = point_line_distance(*point, points[0], points[points.len() - 1]);
        if distance > maximum {
            maximum = distance;
            split = index + 1;
        }
    }
    if maximum <= tolerance {
        return vec![points[0], points[points.len() - 1]];
    }
    let mut left = rdp(&points[..=split], tolerance);
    let right = rdp(&points[split..], tolerance);
    left.pop();
    left.extend(right);
    left
}

fn point_line_distance(point: Vec2, first: Vec2, second: Vec2) -> f64 {
    let dx = second.x - first.x;
    let dy = second.y - first.y;
    if dx == 0.0 && dy == 0.0 {
        return distance_squared(point, first).sqrt();
    }
    ((dy * point.x - dx * point.y + second.x * first.y - second.y * first.x).abs())
        / (dx * dx + dy * dy).sqrt()
}

fn distance_squared(first: Vec2, second: Vec2) -> f64 {
    (first.x - second.x).powi(2) + (first.y - second.y).powi(2)
}

fn polygon_twice_area(points: &[Vec2]) -> f64 {
    (0..points.len())
        .map(|index| {
            let first = points[index];
            let second = points[(index + 1) % points.len()];
            first.x * second.y - second.x * first.y
        })
        .sum()
}

struct PreparedExposure {
    grid: BitGrid,
    layer: LayerSlice,
    warnings: Vec<String>,
}

fn validate_config(config: &PcbExposureConfig) -> Result<()> {
    if !config.exposure_s.is_finite() || config.exposure_s <= 0.0 {
        return Err(invalid(
            "exposure time must be finite and greater than zero",
        ));
    }
    if !config.layer_height_mm.is_finite() || config.layer_height_mm <= 0.0 {
        return Err(invalid("layer height must be finite and greater than zero"));
    }
    if !config.lift_height_mm.is_finite() || config.lift_height_mm < 0.0 {
        return Err(invalid("lift height must be finite and non-negative"));
    }
    if !config.lift_speed_mm_s.is_finite() || config.lift_speed_mm_s <= 0.0 {
        return Err(invalid("lift speed must be finite and greater than zero"));
    }
    if !config.scale.is_finite() || config.scale <= 0.0 {
        return Err(invalid("PCB scale must be finite and greater than zero"));
    }
    if !config.rotation_deg.is_finite()
        || config.offset_mm.iter().any(|value| !value.is_finite())
        || !config.invert_margin_mm.is_finite()
        || config.invert_margin_mm < 0.0
        || !config.simplify_tolerance_mm.is_finite()
        || config.simplify_tolerance_mm < 0.0
    {
        return Err(invalid("invalid PCB transform or simplification value"));
    }
    if config
        .machine
        .build_volume_mm
        .iter()
        .any(|value| !value.is_finite() || *value <= 0.0)
        || config.machine.resolution.contains(&0)
    {
        return Err(invalid(
            "machine build volume and resolution must be positive",
        ));
    }
    Ok(())
}

fn prepare_exposure(
    gerber_paths: &[PathBuf],
    drill_paths: &[PathBuf],
    config: &PcbExposureConfig,
) -> Result<PreparedExposure> {
    validate_config(config)?;
    if gerber_paths.is_empty() {
        return Err(invalid("at least one Gerber file is required"));
    }
    let automatically_mirrored =
        config.auto_mirror_front_layers && gerber_paths.iter().any(filename_suggests_front_mirror);
    let mut gerbers = gerber_paths
        .iter()
        .map(|path| parse_gerber(path))
        .collect::<Result<Vec<_>>>()?;
    let mut drills = drill_paths
        .iter()
        .map(|path| parse_excellon(path))
        .collect::<Result<Vec<_>>>()?;
    let source_bounds = gerbers
        .iter()
        .chain(&drills)
        .filter_map(|artwork| operations_bounds(&artwork.operations))
        .fold(None, |combined: Option<[f64; 4]>, bounds| {
            Some(match combined {
                None => bounds,
                Some(current) => [
                    current[0].min(bounds[0]),
                    current[1].min(bounds[1]),
                    current[2].max(bounds[2]),
                    current[3].max(bounds[3]),
                ],
            })
        })
        .ok_or_else(|| invalid("artwork has no measurable bounds"))?;
    let transform = ArtworkTransform {
        source_center: Point {
            x: (source_bounds[0] + source_bounds[2]) * 0.5,
            y: (source_bounds[1] + source_bounds[3]) * 0.5,
        },
        target_center: Point {
            x: config.offset_mm[0],
            y: config.offset_mm[1],
        },
        radians: config.rotation_deg.to_radians(),
        scale: config.scale,
        mirror_x: config.mirror_x || automatically_mirrored,
        mirror_y: config.mirror_y,
    };
    for artwork in gerbers.iter_mut().chain(&mut drills) {
        for operation in &mut artwork.operations {
            operation.shape = operation.shape.transformed(&transform);
        }
    }
    let transformed_bounds = gerbers
        .iter()
        .chain(&drills)
        .filter_map(|artwork| operations_bounds(&artwork.operations))
        .fold(None, |combined: Option<[f64; 4]>, bounds| {
            Some(match combined {
                None => bounds,
                Some(current) => [
                    current[0].min(bounds[0]),
                    current[1].min(bounds[1]),
                    current[2].max(bounds[2]),
                    current[3].max(bounds[3]),
                ],
            })
        })
        .ok_or_else(|| invalid("transformed artwork is empty"))?;
    let half_x = config.machine.build_volume_mm[0] * 0.5;
    let half_y = config.machine.build_volume_mm[1] * 0.5;
    if transformed_bounds[0] < -half_x
        || transformed_bounds[2] > half_x
        || transformed_bounds[1] < -half_y
        || transformed_bounds[3] > half_y
    {
        return Err(invalid(format!(
            "PCB bounds [{:.3}, {:.3}] x [{:.3}, {:.3}] mm exceed the centered {:.3} x {:.3} mm LCD",
            transformed_bounds[0],
            transformed_bounds[2],
            transformed_bounds[1],
            transformed_bounds[3],
            config.machine.build_volume_mm[0],
            config.machine.build_volume_mm[1]
        )));
    }

    let mut grid = BitGrid::new(&config.machine)?;
    let mut active_rect = None::<PixelRect>;
    for gerber in &gerbers {
        let mut layer_grid = BitGrid::new(&config.machine)?;
        let mut layer_rect = None::<PixelRect>;
        for operation in &gerber.operations {
            if let Some(rect) = raster_operation(&mut layer_grid, operation) {
                layer_rect = Some(layer_rect.map_or(rect, |current| current.union(rect)));
            }
        }
        if gerber.negative
            && let Some(bounds) = operations_bounds(&gerber.operations)
            && let Some(rect) = invert_rect(&mut layer_grid, bounds)
        {
            layer_rect = Some(layer_rect.map_or(rect, |current| current.union(rect)));
        }
        grid.or_assign(&layer_grid);
        if let Some(rect) = layer_rect {
            active_rect = Some(active_rect.map_or(rect, |current| current.union(rect)));
        }
    }
    if config.invert {
        let bounds = [
            transformed_bounds[0] - config.invert_margin_mm,
            transformed_bounds[1] - config.invert_margin_mm,
            transformed_bounds[2] + config.invert_margin_mm,
            transformed_bounds[3] + config.invert_margin_mm,
        ];
        if bounds[0] < -half_x || bounds[2] > half_x || bounds[1] < -half_y || bounds[3] > half_y {
            return Err(invalid(
                "inverted artwork rectangle plus margin exceeds the LCD",
            ));
        }
        if let Some(rect) = invert_rect(&mut grid, bounds) {
            active_rect = Some(active_rect.map_or(rect, |current| current.union(rect)));
        }
    }
    if config.drill_mode != DrillMode::Ignore {
        for drill in &drills {
            for source in &drill.operations {
                let operation = Operation {
                    shape: source.shape.clone(),
                    dark: config.drill_mode == DrillMode::Expose,
                };
                if let Some(rect) = raster_operation(&mut grid, &operation) {
                    active_rect = Some(active_rect.map_or(rect, |current| current.union(rect)));
                }
            }
        }
    }
    let active_rect = active_rect.ok_or_else(|| invalid("artwork raster is empty"))?;
    let exposed_pixels = grid.count();
    if exposed_pixels == 0 {
        return Err(invalid(
            "the final exposure is empty after polarity and drill composition",
        ));
    }
    let contours = bitmap_contours(&grid, active_rect, config.simplify_tolerance_mm);
    if contours.is_empty() {
        return Err(invalid(
            "could not trace an outline around exposed LCD pixels",
        ));
    }
    let layer = layer_from_contours(0, config.layer_height_mm * 0.5, contours);
    if layer.segments.len() > config.machine.max_segments_per_layer {
        return Err(invalid(format!(
            "PCB layer contains {} vector segments, exceeding machine limit {}; increase --simplify",
            layer.segments.len(),
            config.machine.max_segments_per_layer
        )));
    }
    let mut warnings = gerbers
        .into_iter()
        .chain(drills)
        .flat_map(|artwork| artwork.warnings)
        .collect::<HashSet<_>>()
        .into_iter()
        .collect::<Vec<_>>();
    if automatically_mirrored && !config.mirror_x {
        warnings.push(
            "automatically mirrored X because a Gerber filename identifies a KiCad front-side layer"
                .to_owned(),
        );
    }
    warnings.sort();
    Ok(PreparedExposure {
        grid,
        layer,
        warnings,
    })
}

fn slicer_config(config: &PcbExposureConfig) -> SlicerConfig {
    SlicerConfig {
        machine: config.machine.clone(),
        print: PrintConfig {
            layer_height_mm: config.layer_height_mm,
            bottom_layers: 1,
            bottom_exposure_s: config.exposure_s,
            exposure_s: config.exposure_s,
            lift_height_mm: config.lift_height_mm,
            lift_speed_mm_s: config.lift_speed_mm_s,
            off_time_s: 0.0,
            ..PrintConfig::default()
        },
        transform: Transform::default(),
        supports: SupportConfig::default(),
        xy_offset_mm: 0.0,
        hollow: false,
        gmsh_path: PathBuf::from("gmsh"),
    }
}

fn exposure_report(
    exposure: &PreparedExposure,
    gerber_files: usize,
    drill_files: usize,
    exposure_s: f64,
    output: &Path,
) -> Result<PcbExposureReport> {
    Ok(PcbExposureReport {
        gerber_files,
        drill_files,
        exposure_s,
        exposed_pixels: exposure.grid.count(),
        contour_count: exposure.layer.contours.len(),
        segment_count: exposure.layer.segments.len(),
        bounds_mm: exposure.layer.bounds,
        exposed_area_mm2: exposure.layer.area(),
        output_bytes: std::fs::metadata(output)?.len(),
        warnings: exposure.warnings.clone(),
    })
}

/// Creates a one-layer PWSZ whose first/bottom exposure is `exposure_s`.
pub fn slice_pcb(
    gerber_paths: &[PathBuf],
    drill_paths: &[PathBuf],
    output: impl AsRef<Path>,
    config: &PcbExposureConfig,
) -> Result<PcbExposureReport> {
    let output = output.as_ref();
    let exposure = prepare_exposure(gerber_paths, drill_paths, config)?;
    let slicer = slicer_config(config);
    let previews = render_exposure_previews(&exposure.grid);
    let empty = Mesh {
        triangles: Vec::new(),
    };
    write_pwsz_with_previews(
        output,
        &gerber_paths[0],
        &slicer,
        &empty,
        &empty,
        std::slice::from_ref(&exposure.layer),
        exposure.layer.area() * config.layer_height_mm,
        &previews,
        true,
    )?;
    exposure_report(
        &exposure,
        gerber_paths.len(),
        drill_paths.len(),
        config.exposure_s,
        output,
    )
}

/// Exports the exact composed LCD contour to the Qt viewer's `.acv` format.
pub fn export_pcb_viewer_scene(
    gerber_paths: &[PathBuf],
    drill_paths: &[PathBuf],
    output: impl AsRef<Path>,
    config: &PcbExposureConfig,
) -> Result<PcbExposureReport> {
    let output = output.as_ref();
    let exposure = prepare_exposure(gerber_paths, drill_paths, config)?;
    let empty = Mesh {
        triangles: Vec::new(),
    };
    write_viewer_scene(
        output,
        &slicer_config(config),
        &empty,
        &empty,
        std::slice::from_ref(&exposure.layer),
    )?;
    exposure_report(
        &exposure,
        gerber_paths.len(),
        drill_paths.len(),
        config.exposure_s,
        output,
    )
}

fn render_exposure_previews(grid: &BitGrid) -> Vec<RenderedImage> {
    [
        (224, 168, [2, 72, 100, 255], [72, 225, 217, 255], 0.9),
        (336, 252, [20, 27, 41, 255], [55, 218, 210, 255], 0.9),
        (800, 600, [238, 240, 246, 255], [25, 55, 65, 255], 0.92),
    ]
    .into_iter()
    .map(|(width, height, background, exposed, fill)| {
        render_exposure_preview(grid, width, height, background, exposed, fill)
    })
    .collect()
}

fn render_exposure_preview(
    grid: &BitGrid,
    width: u32,
    height: u32,
    background: [u8; 4],
    exposed: [u8; 4],
    fill: f64,
) -> RenderedImage {
    let scale = (f64::from(width) * fill / grid.physical_width)
        .min(f64::from(height) * fill / grid.physical_height);
    let plate_width = grid.physical_width * scale;
    let plate_height = grid.physical_height * scale;
    let left = (f64::from(width) - plate_width) * 0.5;
    let top = (f64::from(height) - plate_height) * 0.5;
    let mut rgba = background.repeat((width * height) as usize);
    for target_y in 0..height {
        for target_x in 0..width {
            let local_x = (f64::from(target_x) + 0.5 - left) / plate_width;
            let local_y = 1.0 - (f64::from(target_y) + 0.5 - top) / plate_height;
            if !(0.0..1.0).contains(&local_x) || !(0.0..1.0).contains(&local_y) {
                continue;
            }
            let source_x = (local_x * f64::from(grid.width)).floor() as u32;
            let source_y = (local_y * f64::from(grid.height)).floor() as u32;
            if grid.get(source_x.min(grid.width - 1), source_y.min(grid.height - 1)) {
                let index = ((target_y * width + target_x) * 4) as usize;
                rgba[index..index + 4].copy_from_slice(&exposed);
            }
        }
    }
    RenderedImage {
        width,
        height,
        rgba,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(name: &str, contents: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "anycubic-pcb-{}-{name}",
            uuid::Uuid::new_v4().simple()
        ));
        std::fs::write(&path, contents).unwrap();
        path
    }

    fn small_config() -> PcbExposureConfig {
        let mut config = PcbExposureConfig::default();
        config.machine.resolution = [400, 200];
        config.machine.build_volume_mm = [20.0, 10.0, 230.0];
        config.machine.pixel_size_um = [50.0, 50.0];
        config.simplify_tolerance_mm = 0.03;
        config.auto_mirror_front_layers = false;
        config
    }

    #[test]
    fn recognizes_only_named_front_side_production_layers() {
        for name in [
            "board-F_Cu.gbr",
            "board-f_mask.gbr",
            "board.F-Paste.gbr",
            "F_Silkscreen.gto",
        ] {
            assert!(filename_suggests_front_mirror(name), "{name}");
        }
        for name in [
            "board-B_Cu.gbr",
            "board-Edge_Cuts.gbr",
            "board-PTH.drl",
            "my-F_Current.gbr",
        ] {
            assert!(!filename_suggests_front_mirror(name), "{name}");
        }
    }

    #[test]
    fn front_layer_name_enables_auto_mirror_and_reports_it() {
        let gerber = fixture(
            "board-F_Cu.gbr",
            "%FSLAX24Y24*%\n%MOMM*%\n%ADD10R,2.0X1.0*%\nD10*\nX020000Y010000D03*\nM02*\n",
        );
        let mut config = small_config();
        config.auto_mirror_front_layers = true;
        let prepared = prepare_exposure(std::slice::from_ref(&gerber), &[], &config).unwrap();
        std::fs::remove_file(gerber).unwrap();
        assert!(
            prepared
                .warnings
                .iter()
                .any(|warning| warning.contains("automatically mirrored X"))
        );
    }

    #[test]
    fn parses_kicad_metric_coordinates_and_aperture() {
        let gerber = fixture(
            "trace.gbr",
            "%FSLAX24Y24*%\n%MOMM*%\n%ADD10C,0.200*%\nD10*\nX000000Y000000D02*\nX100000Y000000D01*\nM02*\n",
        );
        let parsed = parse_gerber(&gerber).unwrap();
        std::fs::remove_file(gerber).unwrap();
        assert_eq!(parsed.operations.len(), 1);
        let Shape::Stroke { points, width } = &parsed.operations[0].shape else {
            panic!("expected Gerber stroke");
        };
        assert!((points[1].x - 10.0).abs() < 1.0e-9);
        assert!((*width - 0.2).abs() < 1.0e-9);
    }

    #[test]
    fn drill_clear_is_aligned_to_gerber_coordinates() {
        let gerber = fixture(
            "board-F_Cu.gbr",
            "%FSLAX24Y24*%\n%MOMM*%\n%ADD10R,10.0X6.0*%\nD10*\nX100000Y050000D03*\nM02*\n",
        );
        let drill = fixture(
            "board-PTH.drl",
            "M48\nMETRIC,000.000\nT01C1.000\n%\nT01\nX010000Y005000\nM30\n",
        );
        let prepared = prepare_exposure(
            std::slice::from_ref(&gerber),
            std::slice::from_ref(&drill),
            &small_config(),
        )
        .unwrap();
        std::fs::remove_file(gerber).unwrap();
        std::fs::remove_file(drill).unwrap();
        assert_eq!(prepared.layer.contours.len(), 2);
        assert!(prepared.layer.area() > 58.0 && prepared.layer.area() < 60.0);
        let center_x = prepared.grid.width / 2;
        let center_y = prepared.grid.height / 2;
        assert!(!prepared.grid.get(center_x, center_y));
        assert!(prepared.grid.get(center_x + 30, center_y));
    }

    #[test]
    fn accepts_an_empty_kicad_npth_file_as_a_noop() {
        let drill = fixture(
            "empty-NPTH.drl",
            "M48\n; #@! TF.FileFunction,NonPlated,1,4,NPTH\nFMAT,2\nMETRIC\n%\nG90\nG05\nM30\n",
        );
        let parsed = parse_excellon(&drill).unwrap();
        std::fs::remove_file(drill).unwrap();
        assert!(parsed.operations.is_empty());
        assert_eq!(parsed.warnings.len(), 1);
        assert!(parsed.warnings[0].contains("contains no drill coordinates"));
    }

    #[test]
    fn writes_a_single_layer_job_with_the_requested_exposure() {
        let gerber = fixture(
            "one-layer.gbr",
            "%FSLAX24Y24*%\n%MOMM*%\n%ADD10R,4.0X2.0*%\nD10*\nX10000Y10000D03*\nM02*\n",
        );
        let output = std::env::temp_dir().join(format!(
            "anycubic-pcb-{}.pwsz",
            uuid::Uuid::new_v4().simple()
        ));
        let mut config = small_config();
        config.exposure_s = 17.5;
        config.lift_height_mm = 0.0;
        let report = slice_pcb(std::slice::from_ref(&gerber), &[], &output, &config).unwrap();
        assert_eq!(report.gerber_files, 1);
        let mut archive = crate::pwsz::PwszArchive::open(&output).unwrap();
        assert_eq!(archive.metadata().layers.count, 1);
        assert_eq!(archive.metadata().preview_indices, vec![0, 1, 2]);
        assert_eq!(archive.metadata().layers.paras[0].exposure_time, 17.5);
        assert_eq!(archive.metadata().layers.paras[0].zup_height, Some(0.0));
        assert_eq!(
            archive.metadata().machine_extern.user_resins[0]["slice_extpara"]["intelli_mode"],
            0
        );
        assert!(
            !archive
                .read_layer(0)
                .unwrap()
                .segments()
                .unwrap()
                .is_empty()
        );
        std::fs::remove_file(gerber).unwrap();
        std::fs::remove_file(output).unwrap();
    }
}
