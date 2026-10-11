use std::{
    collections::{HashMap, HashSet},
    fs::File,
    io::Read,
    path::Path,
};

#[cfg(not(anycubic_occt))]
use std::process::Command;

use zip::ZipArchive;

#[cfg(anycubic_occt)]
use std::ffi::{CStr, CString};

use crate::{Error, Result};

use super::Transform;

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub(crate) struct Vec3 {
    pub x: f64,
    pub y: f64,
    pub z: f64,
}

impl Vec3 {
    pub(super) fn min(self, other: Self) -> Self {
        Self {
            x: self.x.min(other.x),
            y: self.y.min(other.y),
            z: self.z.min(other.z),
        }
    }

    pub(super) fn max(self, other: Self) -> Self {
        Self {
            x: self.x.max(other.x),
            y: self.y.max(other.y),
            z: self.z.max(other.z),
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct Triangle(pub [Vec3; 3]);

#[derive(Debug, Clone)]
pub(crate) struct Mesh {
    pub triangles: Vec<Triangle>,
}

impl Mesh {
    pub fn load(path: &Path, gmsh_path: &Path, tessellation_error_mm: f64) -> Result<Self> {
        let extension = path
            .extension()
            .and_then(|value| value.to_str())
            .unwrap_or_default()
            .to_ascii_lowercase();
        match extension.as_str() {
            "stl" => Self::read_stl(path),
            "3mf" => Self::read_3mf(path),
            "step" | "stp" | "iges" | "igs" => {
                Self::convert_cad(path, gmsh_path, tessellation_error_mm)
            }
            _ => Err(Error::UnsupportedModelFormat(extension)),
        }
    }

    fn read_stl(path: &Path) -> Result<Self> {
        let bytes = std::fs::read(path)?;
        Self::read_stl_bytes(&bytes)
    }

    fn read_stl_bytes(bytes: &[u8]) -> Result<Self> {
        if bytes.len() >= 84 {
            let count = u32::from_le_bytes(bytes[80..84].try_into().expect("fixed range")) as usize;
            if 84_usize
                .checked_add(count.saturating_mul(50))
                .is_some_and(|size| size == bytes.len())
            {
                let mut triangles = Vec::with_capacity(count);
                for record in bytes[84..].chunks_exact(50) {
                    let mut vertices = [Vec3::default(); 3];
                    for (index, vertex) in vertices.iter_mut().enumerate() {
                        let offset = 12 + index * 12;
                        vertex.x = read_f32(record, offset)? as f64;
                        vertex.y = read_f32(record, offset + 4)? as f64;
                        vertex.z = read_f32(record, offset + 8)? as f64;
                    }
                    triangles.push(Triangle(vertices));
                }
                return Self::checked(triangles);
            }
        }

        let text = std::str::from_utf8(bytes).map_err(|_| {
            Error::InvalidModel("STL is neither valid binary nor UTF-8 ASCII".into())
        })?;
        let mut vertices = Vec::new();
        for line in text.lines() {
            let mut words = line.split_whitespace();
            if words.next() != Some("vertex") {
                continue;
            }
            let coordinates = words
                .take(3)
                .map(|word| word.parse::<f64>())
                .collect::<std::result::Result<Vec<_>, _>>()
                .map_err(|error| {
                    Error::InvalidModel(format!("invalid ASCII STL vertex: {error}"))
                })?;
            if coordinates.len() != 3 {
                return Err(Error::InvalidModel("incomplete ASCII STL vertex".into()));
            }
            vertices.push(Vec3 {
                x: coordinates[0],
                y: coordinates[1],
                z: coordinates[2],
            });
        }
        if vertices.len() % 3 != 0 {
            return Err(Error::InvalidModel(
                "ASCII STL vertex count is not divisible by three".into(),
            ));
        }
        Self::checked(
            vertices
                .chunks_exact(3)
                .map(|vertices| Triangle([vertices[0], vertices[1], vertices[2]]))
                .collect(),
        )
    }

    fn convert_cad(path: &Path, gmsh_path: &Path, tessellation_error_mm: f64) -> Result<Self> {
        if !tessellation_error_mm.is_finite() || tessellation_error_mm <= 0.0 {
            return Err(Error::InvalidModel(
                "CAD tessellation error must be finite and positive".into(),
            ));
        }
        #[cfg(anycubic_occt)]
        {
            let _ = gmsh_path;
            Self::convert_cad_occt(path, tessellation_error_mm)
        }
        #[cfg(not(anycubic_occt))]
        {
            Self::convert_cad_gmsh(path, gmsh_path, tessellation_error_mm)
        }
    }

    #[cfg(anycubic_occt)]
    fn convert_cad_occt(path: &Path, tessellation_error_mm: f64) -> Result<Self> {
        let output = std::env::temp_dir().join(format!(
            "anycubicstudio-{}.stl",
            uuid::Uuid::new_v4().simple()
        ));
        let input = CString::new(path.to_string_lossy().as_bytes()).map_err(|_| {
            Error::MeshConverter("CAD input path contains an embedded NUL byte".into())
        })?;
        let output_text = CString::new(output.to_string_lossy().as_bytes()).map_err(|_| {
            Error::MeshConverter("CAD output path contains an embedded NUL byte".into())
        })?;
        let mut error = vec![0_i8; 2048];
        // About seven degrees prevents visible angular facets while chordal
        // deflection remains tied to the printer pixel size.
        let status = unsafe {
            anycubic_occt_tessellate(
                input.as_ptr(),
                output_text.as_ptr(),
                tessellation_error_mm,
                0.12,
                error.as_mut_ptr(),
                error.len(),
            )
        };
        if status != 0 {
            let message = unsafe { CStr::from_ptr(error.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            return Err(Error::MeshConverter(if message.is_empty() {
                format!("OpenCascade tessellation failed with status {status}")
            } else {
                message
            }));
        }
        let converted = Self::read_stl(&output);
        if let Some(debug_output) = std::env::var_os("ANYCUBIC_OCCT_DEBUG_STL") {
            let _ = std::fs::copy(&output, debug_output);
        }
        let _ = std::fs::remove_file(&output);
        converted
    }

    #[cfg(not(anycubic_occt))]
    fn convert_cad_gmsh(path: &Path, gmsh_path: &Path, tessellation_error_mm: f64) -> Result<Self> {
        let output = std::env::temp_dir().join(format!(
            "anycubicstudio-{}.stl",
            uuid::Uuid::new_v4().simple()
        ));
        let converted = (|| {
            // The inexpensive first pass supplies the physical size needed to
            // choose a scale-aware surface mesh. A fixed global edge size is
            // either visibly faceted on large curved parts or wasteful on
            // small detailed ones.
            run_gmsh(gmsh_path, path, &output, &[])?;
            let coarse = Self::read_stl(&output)?;
            let (minimum, maximum) = coarse.bounds();
            let diagonal = ((maximum.x - minimum.x).powi(2)
                + (maximum.y - minimum.y).powi(2)
                + (maximum.z - minimum.z).powi(2))
            .sqrt();
            if !diagonal.is_finite() || diagonal <= 0.0 {
                return Err(Error::InvalidModel(
                    "CAD model has no finite physical extent".into(),
                ));
            }

            // For a circular arc, chord ~= sqrt(8 * radius * sagitta).
            // Using diagonal/2 as the largest representative radius gives a
            // useful maximum edge while Gmsh's curvature sizing refines small
            // radii. The lower bound prevents microscopic triangles that the
            // target LCD cannot resolve.
            let minimum_edge = (tessellation_error_mm * 8.0).max(diagonal * 1.0e-5);
            let maximum_edge = (4.0 * diagonal * tessellation_error_mm)
                .sqrt()
                .max(minimum_edge)
                .min((diagonal / 8.0).max(minimum_edge));
            let mesh_minimum = (maximum_edge / 20.0)
                .max(tessellation_error_mm * 2.0)
                .min(maximum_edge);
            let maximum_text = maximum_edge.to_string();
            let minimum_text = mesh_minimum.to_string();
            let options = [
                ("Mesh.MeshSizeFromCurvature", "24"),
                ("Mesh.MinimumCirclePoints", "24"),
                ("Mesh.MinimumCurvePoints", "8"),
                ("Mesh.MeshSizeMax", maximum_text.as_str()),
                ("Mesh.MeshSizeMin", minimum_text.as_str()),
                ("Mesh.Optimize", "1"),
                ("Geometry.OCCFixSmallEdges", "1"),
            ];
            run_gmsh(gmsh_path, path, &output, &options)?;
            Self::read_stl(&output)
        })();
        let _ = std::fs::remove_file(&output);
        converted
    }

    /// Reads the mesh subset of the 3MF Core specification, including build
    /// items, component objects, object/build transforms and model units.
    fn read_3mf(path: &Path) -> Result<Self> {
        let mut archive = ZipArchive::new(File::open(path)?)?;
        let model_name = match archive.by_name("_rels/.rels") {
            Ok(mut relationships) => {
                let mut xml = String::new();
                relationships.read_to_string(&mut xml)?;
                xml_tags(&xml)?.into_iter().find_map(|tag| {
                    let kind = tag.attributes.get("Type")?;
                    let target = tag.attributes.get("Target")?;
                    kind.to_ascii_lowercase()
                        .ends_with("/3dmodel")
                        .then(|| target.trim_start_matches('/').replace('\\', "/"))
                })
            }
            Err(zip::result::ZipError::FileNotFound) => None,
            Err(error) => return Err(error.into()),
        };
        let model_name = model_name.or_else(|| {
            (0..archive.len()).find_map(|index| {
                let file = archive.by_index(index).ok()?;
                let name = file.name().to_owned();
                name.to_ascii_lowercase()
                    .ends_with(".model")
                    .then_some(name)
            })
        });
        let model_name = model_name
            .ok_or_else(|| Error::InvalidModel("3MF archive contains no .model XML part".into()))?;
        let mut xml = String::new();
        archive.by_name(&model_name)?.read_to_string(&mut xml)?;
        Model3mf::parse(&xml)?.into_mesh()
    }

    fn checked(triangles: Vec<Triangle>) -> Result<Self> {
        if triangles.is_empty() {
            return Err(Error::InvalidModel("mesh contains no triangles".into()));
        }
        if triangles
            .iter()
            .flat_map(|triangle| triangle.0)
            .any(|point| !point.x.is_finite() || !point.y.is_finite() || !point.z.is_finite())
        {
            return Err(Error::InvalidModel(
                "mesh contains a non-finite coordinate".into(),
            ));
        }
        Ok(Self { triangles })
    }

    /// Snaps sub-resolution cracks to a shared 3D grid and removes triangles
    /// that collapse or are exact duplicates after snapping. This is topology
    /// repair within a caller-selected physical tolerance, not permission to
    /// ignore a genuinely open shell.
    pub fn repair_for_slicing(&mut self, tolerance_mm: f64) -> Result<()> {
        if !tolerance_mm.is_finite() || tolerance_mm <= 0.0 {
            return Err(Error::InvalidModel(
                "mesh repair tolerance must be finite and positive".into(),
            ));
        }
        let original_count = self.triangles.len();
        let inverse = 1.0 / tolerance_mm;
        let mut unique = HashSet::<[VertexKey; 3]>::with_capacity(original_count);
        let minimum_area_vector_squared = tolerance_mm.powi(4) * 1.0e-4;
        self.triangles.retain_mut(|triangle| {
            let mut keys = [VertexKey::default(); 3];
            for (point, key) in triangle.0.iter_mut().zip(&mut keys) {
                *key = VertexKey::from_point(*point, inverse);
                *point = key.to_point(tolerance_mm);
            }
            if keys[0] == keys[1] || keys[1] == keys[2] || keys[2] == keys[0] {
                return false;
            }
            let first = triangle.0[1] - triangle.0[0];
            let second = triangle.0[2] - triangle.0[0];
            if first.cross(second).length_squared() <= minimum_area_vector_squared {
                return false;
            }
            keys.sort_unstable();
            unique.insert(keys)
        });
        if self.triangles.is_empty() {
            return Err(Error::InvalidModel(
                "mesh contains no non-degenerate triangles after repair".into(),
            ));
        }
        if std::env::var_os("ANYCUBIC_OCCT_DIAGNOSTICS").is_some() {
            eprintln!(
                "mesh repair: {} input triangles, {} retained at {:.9} mm tolerance",
                original_count,
                self.triangles.len(),
                tolerance_mm
            );
        }
        Ok(())
    }

    pub fn bounds(&self) -> (Vec3, Vec3) {
        let first = self.triangles[0].0[0];
        self.triangles
            .iter()
            .flat_map(|triangle| triangle.0)
            .fold((first, first), |(min, max), point| {
                (min.min(point), max.max(point))
            })
    }

    pub fn apply_transform(&mut self, transform: &Transform) -> Result<()> {
        if transform
            .scale
            .iter()
            .any(|value| !value.is_finite() || *value == 0.0)
        {
            return Err(Error::InvalidModel(
                "all scale values must be finite and non-zero".into(),
            ));
        }
        let (min, max) = self.bounds();
        let center = Vec3 {
            x: (min.x + max.x) * 0.5,
            y: (min.y + max.y) * 0.5,
            z: (min.z + max.z) * 0.5,
        };
        let rotation = transform.rotation_deg.map(f64::to_radians);
        for point in self
            .triangles
            .iter_mut()
            .flat_map(|triangle| triangle.0.iter_mut())
        {
            point.x = (point.x - center.x) * transform.scale[0];
            point.y = (point.y - center.y) * transform.scale[1];
            point.z = (point.z - center.z) * transform.scale[2];
            *point = rotate_xyz(*point, rotation);
        }

        let (min, max) = self.bounds();
        let base_translation = Vec3 {
            x: if transform.auto_center_xy {
                -(min.x + max.x) * 0.5
            } else {
                center.x
            },
            y: if transform.auto_center_xy {
                -(min.y + max.y) * 0.5
            } else {
                center.y
            },
            z: if transform.place_on_bed {
                -min.z
            } else {
                center.z
            },
        };
        for point in self
            .triangles
            .iter_mut()
            .flat_map(|triangle| triangle.0.iter_mut())
        {
            point.x += base_translation.x + transform.translation_mm[0];
            point.y += base_translation.y + transform.translation_mm[1];
            point.z += base_translation.z + transform.translation_mm[2];
        }
        if transform.scale.iter().product::<f64>() < 0.0 {
            for triangle in &mut self.triangles {
                triangle.0.swap(1, 2);
            }
        }
        Ok(())
    }
}

#[cfg(anycubic_occt)]
unsafe extern "C" {
    fn anycubic_occt_tessellate(
        input: *const std::ffi::c_char,
        output: *const std::ffi::c_char,
        linear_deflection: f64,
        angular_deflection: f64,
        error: *mut std::ffi::c_char,
        error_capacity: usize,
    ) -> std::ffi::c_int;
}

#[cfg(not(anycubic_occt))]
fn run_gmsh(gmsh_path: &Path, input: &Path, output: &Path, options: &[(&str, &str)]) -> Result<()> {
    let mut command = Command::new(gmsh_path);
    command
        .arg(input)
        .args(["-2", "-format", "stl", "-bin", "-o"])
        .arg(output)
        .args(["-v", "1"]);
    for (name, value) in options {
        command.args(["-setnumber", name, value]);
    }
    let result = command.output().map_err(|error| {
        Error::MeshConverter(format!("could not start {}: {error}", gmsh_path.display()))
    })?;
    if !result.status.success() {
        return Err(Error::MeshConverter(format!(
            "{} exited with {}; stderr: {}",
            gmsh_path.display(),
            result.status,
            String::from_utf8_lossy(&result.stderr).trim()
        )));
    }
    Ok(())
}

impl std::ops::Sub for Vec3 {
    type Output = Self;

    fn sub(self, other: Self) -> Self {
        Self {
            x: self.x - other.x,
            y: self.y - other.y,
            z: self.z - other.z,
        }
    }
}

impl Vec3 {
    fn cross(self, other: Self) -> Self {
        Self {
            x: self.y * other.z - self.z * other.y,
            y: self.z * other.x - self.x * other.z,
            z: self.x * other.y - self.y * other.x,
        }
    }

    fn length_squared(self) -> f64 {
        self.x * self.x + self.y * self.y + self.z * self.z
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
struct VertexKey(i64, i64, i64);

impl VertexKey {
    fn from_point(point: Vec3, inverse_tolerance: f64) -> Self {
        Self(
            (point.x * inverse_tolerance).round() as i64,
            (point.y * inverse_tolerance).round() as i64,
            (point.z * inverse_tolerance).round() as i64,
        )
    }

    fn to_point(self, tolerance: f64) -> Vec3 {
        Vec3 {
            x: self.0 as f64 * tolerance,
            y: self.1 as f64 * tolerance,
            z: self.2 as f64 * tolerance,
        }
    }
}

fn read_f32(bytes: &[u8], offset: usize) -> Result<f32> {
    let value = bytes
        .get(offset..offset + 4)
        .ok_or_else(|| Error::InvalidModel("truncated binary STL".into()))?;
    Ok(f32::from_le_bytes(value.try_into().expect("fixed range")))
}

fn rotate_xyz(mut point: Vec3, rotation: [f64; 3]) -> Vec3 {
    let (sin, cos) = rotation[0].sin_cos();
    point = Vec3 {
        x: point.x,
        y: point.y * cos - point.z * sin,
        z: point.y * sin + point.z * cos,
    };
    let (sin, cos) = rotation[1].sin_cos();
    point = Vec3 {
        x: point.x * cos + point.z * sin,
        y: point.y,
        z: -point.x * sin + point.z * cos,
    };
    let (sin, cos) = rotation[2].sin_cos();
    Vec3 {
        x: point.x * cos - point.y * sin,
        y: point.x * sin + point.y * cos,
        z: point.z,
    }
}

#[derive(Debug, Clone, Copy)]
struct Affine([[f64; 4]; 4]);

impl Affine {
    const IDENTITY: Self = Self([
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]);

    fn parse(value: Option<&String>) -> Result<Self> {
        let Some(value) = value else {
            return Ok(Self::IDENTITY);
        };
        let values = value
            .split_whitespace()
            .map(str::parse::<f64>)
            .collect::<std::result::Result<Vec<_>, _>>()
            .map_err(|error| Error::InvalidModel(format!("invalid 3MF transform: {error}")))?;
        if values.len() != 12 {
            return Err(Error::InvalidModel(
                "3MF transform must contain 12 numbers".into(),
            ));
        }
        // 3MF stores a row-vector 4x3 affine matrix. Convert it to the
        // conventional column-vector matrix used below.
        Ok(Self([
            [values[0], values[3], values[6], values[9]],
            [values[1], values[4], values[7], values[10]],
            [values[2], values[5], values[8], values[11]],
            [0.0, 0.0, 0.0, 1.0],
        ]))
    }

    fn then(self, child: Self) -> Self {
        let mut result = [[0.0; 4]; 4];
        for (row, values) in result.iter_mut().enumerate() {
            for (column, value) in values.iter_mut().enumerate() {
                *value = (0..4)
                    .map(|index| self.0[row][index] * child.0[index][column])
                    .sum();
            }
        }
        Self(result)
    }

    fn point(self, point: Vec3, scale: f64) -> Vec3 {
        let point = [point.x, point.y, point.z, 1.0];
        Vec3 {
            x: (0..4)
                .map(|index| self.0[0][index] * point[index])
                .sum::<f64>()
                * scale,
            y: (0..4)
                .map(|index| self.0[1][index] * point[index])
                .sum::<f64>()
                * scale,
            z: (0..4)
                .map(|index| self.0[2][index] * point[index])
                .sum::<f64>()
                * scale,
        }
    }
}

#[derive(Default)]
struct Object3mf {
    vertices: Vec<Vec3>,
    triangles: Vec<[usize; 3]>,
    components: Vec<(u32, Affine)>,
}

struct Model3mf {
    unit_scale_mm: f64,
    objects: HashMap<u32, Object3mf>,
    build: Vec<(u32, Affine)>,
}

impl Model3mf {
    fn parse(xml: &str) -> Result<Self> {
        let mut objects = HashMap::<u32, Object3mf>::new();
        let mut build = Vec::new();
        let mut current_object = None;
        let mut unit_scale_mm = 1.0;

        for tag in xml_tags(xml)? {
            let name = tag.name.rsplit(':').next().unwrap_or(&tag.name);
            if tag.closing {
                if name == "object" {
                    current_object = None;
                }
                continue;
            }
            match name {
                "model" => {
                    if tag
                        .attributes
                        .get("requiredextensions")
                        .is_some_and(|extensions| !extensions.trim().is_empty())
                    {
                        return Err(Error::InvalidModel(
                            "3MF declares required extensions that this Core mesh reader does not implement"
                                .into(),
                        ));
                    }
                    unit_scale_mm = match tag.attributes.get("unit").map(String::as_str) {
                        None | Some("millimeter") => 1.0,
                        Some("micron") => 0.001,
                        Some("centimeter") => 10.0,
                        Some("inch") => 25.4,
                        Some("foot") => 304.8,
                        Some("meter") => 1000.0,
                        Some(unit) => {
                            return Err(Error::InvalidModel(format!(
                                "unsupported 3MF unit {unit}"
                            )));
                        }
                    };
                }
                "object" => {
                    let id = attr_u32(&tag.attributes, "id")?;
                    objects.entry(id).or_default();
                    current_object = Some(id);
                }
                "vertex" => {
                    let id = current_object
                        .ok_or_else(|| Error::InvalidModel("3MF vertex outside object".into()))?;
                    objects
                        .get_mut(&id)
                        .expect("inserted object")
                        .vertices
                        .push(Vec3 {
                            x: attr_f64(&tag.attributes, "x")?,
                            y: attr_f64(&tag.attributes, "y")?,
                            z: attr_f64(&tag.attributes, "z")?,
                        });
                }
                "triangle" => {
                    let id = current_object
                        .ok_or_else(|| Error::InvalidModel("3MF triangle outside object".into()))?;
                    objects
                        .get_mut(&id)
                        .expect("inserted object")
                        .triangles
                        .push([
                            attr_u32(&tag.attributes, "v1")? as usize,
                            attr_u32(&tag.attributes, "v2")? as usize,
                            attr_u32(&tag.attributes, "v3")? as usize,
                        ]);
                }
                "component" => {
                    let id = current_object.ok_or_else(|| {
                        Error::InvalidModel("3MF component outside object".into())
                    })?;
                    objects
                        .get_mut(&id)
                        .expect("inserted object")
                        .components
                        .push((
                            attr_u32(&tag.attributes, "objectid")?,
                            Affine::parse(tag.attributes.get("transform"))?,
                        ));
                }
                "item" => build.push((
                    attr_u32(&tag.attributes, "objectid")?,
                    Affine::parse(tag.attributes.get("transform"))?,
                )),
                _ => {}
            }
        }

        Ok(Self {
            unit_scale_mm,
            objects,
            build,
        })
    }

    fn into_mesh(self) -> Result<Mesh> {
        if self.build.is_empty() {
            return Err(Error::InvalidModel(
                "3MF model has no build items to manufacture".into(),
            ));
        }
        let roots = self.build.clone();
        let mut triangles = Vec::new();
        for (id, transform) in roots {
            self.expand_object(id, transform, &mut HashSet::new(), &mut triangles)?;
        }
        Mesh::checked(triangles)
    }

    fn expand_object(
        &self,
        id: u32,
        transform: Affine,
        stack: &mut HashSet<u32>,
        output: &mut Vec<Triangle>,
    ) -> Result<()> {
        if !stack.insert(id) {
            return Err(Error::InvalidModel("cyclic 3MF component graph".into()));
        }
        let object = self
            .objects
            .get(&id)
            .ok_or_else(|| Error::InvalidModel(format!("3MF references missing object {id}")))?;
        for indices in &object.triangles {
            let mut vertices = [Vec3::default(); 3];
            for (target, index) in vertices.iter_mut().zip(indices) {
                let point = object.vertices.get(*index).ok_or_else(|| {
                    Error::InvalidModel(format!("3MF triangle references missing vertex {index}"))
                })?;
                *target = transform.point(*point, self.unit_scale_mm);
            }
            output.push(Triangle(vertices));
        }
        for (child, child_transform) in &object.components {
            self.expand_object(*child, transform.then(*child_transform), stack, output)?;
        }
        stack.remove(&id);
        Ok(())
    }
}

struct XmlTag {
    name: String,
    attributes: HashMap<String, String>,
    closing: bool,
}

fn xml_tags(xml: &str) -> Result<Vec<XmlTag>> {
    let mut result = Vec::new();
    let mut position = 0;
    while let Some(relative_start) = xml[position..].find('<') {
        let start = position + relative_start;
        let Some(relative_end) = xml[start..].find('>') else {
            return Err(Error::InvalidModel("unterminated 3MF XML tag".into()));
        };
        let end = start + relative_end;
        position = end + 1;
        let mut body = xml[start + 1..end].trim();
        if body.starts_with('?') || body.starts_with('!') {
            continue;
        }
        let closing = body.starts_with('/');
        if closing {
            body = body[1..].trim_start();
        }
        body = body.trim_end_matches('/').trim_end();
        let name_end = body.find(char::is_whitespace).unwrap_or(body.len());
        let name = body[..name_end].to_owned();
        let attributes = parse_attributes(&body[name_end..])?;
        result.push(XmlTag {
            name,
            attributes,
            closing,
        });
    }
    Ok(result)
}

fn parse_attributes(mut text: &str) -> Result<HashMap<String, String>> {
    let mut attributes = HashMap::new();
    loop {
        text = text.trim_start();
        if text.is_empty() {
            break;
        }
        let equals = text
            .find('=')
            .ok_or_else(|| Error::InvalidModel("invalid 3MF XML attribute".into()))?;
        let key = text[..equals]
            .trim()
            .rsplit(':')
            .next()
            .unwrap_or_default()
            .to_owned();
        text = text[equals + 1..].trim_start();
        let quote = text
            .chars()
            .next()
            .filter(|character| *character == '\'' || *character == '"')
            .ok_or_else(|| Error::InvalidModel("unquoted 3MF XML attribute".into()))?;
        text = &text[quote.len_utf8()..];
        let end = text
            .find(quote)
            .ok_or_else(|| Error::InvalidModel("unterminated 3MF XML attribute".into()))?;
        attributes.insert(key, text[..end].to_owned());
        text = &text[end + quote.len_utf8()..];
    }
    Ok(attributes)
}

fn attr_u32(attributes: &HashMap<String, String>, name: &str) -> Result<u32> {
    attributes
        .get(name)
        .ok_or_else(|| Error::InvalidModel(format!("3MF tag has no {name} attribute")))?
        .parse()
        .map_err(|error| Error::InvalidModel(format!("invalid 3MF {name}: {error}")))
}

fn attr_f64(attributes: &HashMap<String, String>, name: &str) -> Result<f64> {
    attributes
        .get(name)
        .ok_or_else(|| Error::InvalidModel(format!("3MF tag has no {name} attribute")))?
        .parse()
        .map_err(|error| Error::InvalidModel(format!("invalid 3MF {name}: {error}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_ascii_stl() {
        let mesh = Mesh::read_stl_bytes(
            b"solid x\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid\n",
        )
        .unwrap();
        assert_eq!(mesh.triangles.len(), 1);
    }

    #[test]
    fn parses_3mf_core_mesh_and_build_transform() {
        let xml = r#"<model unit="centimeter"><resources><object id="1"><mesh>
          <vertices><vertex x="0" y="0" z="0"/><vertex x="1" y="0" z="0"/><vertex x="0" y="1" z="0"/></vertices>
          <triangles><triangle v1="0" v2="1" v3="2"/></triangles>
        </mesh></object></resources><build><item objectid="1" transform="1 0 0 0 1 0 0 0 1 2 0 0"/></build></model>"#;
        let mesh = Model3mf::parse(xml).unwrap().into_mesh().unwrap();
        assert_eq!(mesh.triangles.len(), 1);
        assert_eq!(mesh.triangles[0].0[0].x, 20.0);
        assert_eq!(mesh.triangles[0].0[1].x, 30.0);
    }

    #[test]
    fn mirroring_repairs_triangle_winding() {
        let mut mesh = Mesh::read_stl_bytes(
            b"solid x\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid\n",
        )
        .unwrap();
        let transform = Transform {
            scale: [-1.0, 1.0, 1.0],
            ..Transform::default()
        };
        mesh.apply_transform(&transform).unwrap();
        let points = mesh.triangles[0].0;
        let cross_z = (points[1].x - points[0].x) * (points[2].y - points[0].y)
            - (points[1].y - points[0].y) * (points[2].x - points[0].x);
        assert!(cross_z > 0.0);
    }
}
