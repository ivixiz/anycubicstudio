use std::collections::HashMap;

use crate::{Error, Result};

use super::mesh::{Mesh, Triangle};

const STITCH_TOLERANCE_MM: f64 = 1.0e-5;
const MIN_SEGMENT_LENGTH_SQUARED: f64 = 1.0e-16;

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub(crate) struct Vec2 {
    pub x: f64,
    pub y: f64,
}

#[derive(Debug, Clone, Copy)]
pub(crate) struct EncodedSegment {
    pub start: Vec2,
    pub end: Vec2,
    pub direction: bool,
}

#[derive(Debug, Clone)]
pub(crate) struct LayerSlice {
    pub index: u32,
    pub z: f64,
    pub contours: Vec<Vec<Vec2>>,
    pub segments: Vec<EncodedSegment>,
    pub signed_twice_area: f64,
    pub bounds: [f64; 4],
}

impl LayerSlice {
    pub fn area(&self) -> f64 {
        self.signed_twice_area.abs() * 0.5
    }
}

pub(crate) fn slice_mesh(mesh: &Mesh, layer_height: f64) -> Result<Vec<LayerSlice>> {
    if !layer_height.is_finite() || layer_height <= 0.0 {
        return Err(Error::InvalidModel(
            "layer height must be finite and greater than zero".into(),
        ));
    }
    let (minimum, maximum) = mesh.bounds();
    if minimum.z < -STITCH_TOLERANCE_MM {
        return Err(Error::InvalidModel(format!(
            "model extends below the build plate ({:.6} mm)",
            minimum.z
        )));
    }
    let layer_count = (maximum.z / layer_height).ceil() as usize;
    if layer_count == 0 {
        return Err(Error::InvalidModel(
            "model has zero printable height".into(),
        ));
    }

    let mut bins = vec![Vec::<usize>::new(); layer_count];
    for (triangle_index, triangle) in mesh.triangles.iter().enumerate() {
        let min_z = triangle
            .0
            .iter()
            .map(|point| point.z)
            .fold(f64::INFINITY, f64::min);
        let max_z = triangle
            .0
            .iter()
            .map(|point| point.z)
            .fold(f64::NEG_INFINITY, f64::max);
        let first_value = ((min_z / layer_height) - 0.5).ceil();
        let last_value = ((max_z / layer_height) - 0.5).floor();
        if last_value < 0.0 || first_value >= layer_count as f64 {
            continue;
        }
        let first = first_value.max(0.0) as usize;
        let last = last_value.min((layer_count - 1) as f64) as usize;
        if first <= last {
            for bin in &mut bins[first..=last] {
                bin.push(triangle_index);
            }
        }
    }

    bins.into_iter()
        .enumerate()
        .map(|(index, candidates)| {
            let z = (index as f64 + 0.5) * layer_height;
            let raw_segments = candidates
                .into_iter()
                .filter_map(|triangle| intersect_triangle(mesh.triangles[triangle], z))
                .collect::<Vec<_>>();
            let mut contours = stitch_segments(&raw_segments).map_err(|message| {
                Error::InvalidModel(format!("layer {index} at z={z:.6} mm: {message}"))
            })?;
            orient_contours(&mut contours);
            Ok(prepare_layer(index as u32, z, contours))
        })
        .collect()
}

fn intersect_triangle(triangle: Triangle, z: f64) -> Option<[Vec2; 2]> {
    let mut intersections = Vec::<Vec2>::with_capacity(3);
    for edge in [[0, 1], [1, 2], [2, 0]] {
        let first = triangle.0[edge[0]];
        let second = triangle.0[edge[1]];
        let first_delta = first.z - z;
        let second_delta = second.z - z;
        if !((first_delta <= 0.0 && second_delta > 0.0)
            || (second_delta <= 0.0 && first_delta > 0.0))
        {
            continue;
        }
        let ratio = (z - first.z) / (second.z - first.z);
        let point = Vec2 {
            x: first.x + (second.x - first.x) * ratio,
            y: first.y + (second.y - first.y) * ratio,
        };
        if !intersections
            .iter()
            .any(|existing| distance_squared(*existing, point) < MIN_SEGMENT_LENGTH_SQUARED)
        {
            intersections.push(point);
        }
    }
    if intersections.len() != 2
        || distance_squared(intersections[0], intersections[1]) < MIN_SEGMENT_LENGTH_SQUARED
    {
        None
    } else {
        Some([intersections[0], intersections[1]])
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct PointKey(i64, i64);

impl PointKey {
    fn new(point: Vec2) -> Self {
        Self(
            (point.x / STITCH_TOLERANCE_MM).round() as i64,
            (point.y / STITCH_TOLERANCE_MM).round() as i64,
        )
    }
}

fn stitch_segments(segments: &[[Vec2; 2]]) -> std::result::Result<Vec<Vec<Vec2>>, String> {
    let mut endpoints = HashMap::<PointKey, Vec<(usize, usize)>>::new();
    for (index, segment) in segments.iter().enumerate() {
        endpoints
            .entry(PointKey::new(segment[0]))
            .or_default()
            .push((index, 0));
        endpoints
            .entry(PointKey::new(segment[1]))
            .or_default()
            .push((index, 1));
    }
    let mut used = vec![false; segments.len()];
    let mut contours = Vec::new();
    for first_segment in 0..segments.len() {
        if used[first_segment] {
            continue;
        }
        used[first_segment] = true;
        let start = segments[first_segment][0];
        let mut current = segments[first_segment][1];
        let start_key = PointKey::new(start);
        let mut contour = vec![start, current];
        while PointKey::new(current) != start_key {
            let key = PointKey::new(current);
            let next = endpoints
                .get(&key)
                .and_then(|candidates| {
                    candidates
                        .iter()
                        .find(|(segment, _)| !used[*segment])
                        .copied()
                })
                .ok_or_else(|| {
                    format!("open contour ends at ({:.6}, {:.6})", current.x, current.y)
                })?;
            used[next.0] = true;
            current = segments[next.0][1 - next.1];
            if PointKey::new(current) != start_key {
                contour.push(current);
            }
            if contour.len() > segments.len() + 1 {
                return Err("contour traversal did not terminate".into());
            }
        }
        remove_duplicate_neighbours(&mut contour);
        if contour.len() >= 3 && twice_area(&contour).abs() > 1.0e-12 {
            contours.push(contour);
        }
    }
    Ok(contours)
}

fn remove_duplicate_neighbours(contour: &mut Vec<Vec2>) {
    contour.dedup_by(|left, right| PointKey::new(*left) == PointKey::new(*right));
    if contour.len() > 1 && PointKey::new(contour[0]) == PointKey::new(*contour.last().unwrap()) {
        contour.pop();
    }
}

fn orient_contours(contours: &mut [Vec<Vec2>]) {
    let original = contours.to_vec();
    for (index, contour) in contours.iter_mut().enumerate() {
        let probe = original[index][0];
        let depth = original
            .iter()
            .enumerate()
            .filter(|(other, polygon)| *other != index && point_in_polygon(probe, polygon))
            .count();
        let should_be_counter_clockwise = depth % 2 == 0;
        if (twice_area(contour) > 0.0) != should_be_counter_clockwise {
            contour.reverse();
        }
    }
}

fn point_in_polygon(point: Vec2, polygon: &[Vec2]) -> bool {
    let mut inside = false;
    for index in 0..polygon.len() {
        let first = polygon[index];
        let second = polygon[(index + 1) % polygon.len()];
        if (first.y > point.y) != (second.y > point.y)
            && point.x < (second.x - first.x) * (point.y - first.y) / (second.y - first.y) + first.x
        {
            inside = !inside;
        }
    }
    inside
}

fn twice_area(contour: &[Vec2]) -> f64 {
    (0..contour.len())
        .map(|index| {
            let first = contour[index];
            let second = contour[(index + 1) % contour.len()];
            first.x * second.y - second.x * first.y
        })
        .sum()
}

fn prepare_layer(index: u32, z: f64, contours: Vec<Vec<Vec2>>) -> LayerSlice {
    let signed_twice_area = contours.iter().map(|contour| twice_area(contour)).sum();
    let bounds = contours
        .iter()
        .flatten()
        .fold(None, |bounds: Option<[f64; 4]>, point| {
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
        .unwrap_or([0.0; 4]);
    let mut segments = contours
        .iter()
        .flat_map(|contour| {
            (0..contour.len()).map(|index| {
                let first = contour[index];
                let second = contour[(index + 1) % contour.len()];
                if first.y <= second.y {
                    EncodedSegment {
                        start: first,
                        end: second,
                        direction: true,
                    }
                } else {
                    EncodedSegment {
                        start: second,
                        end: first,
                        direction: false,
                    }
                }
            })
        })
        .collect::<Vec<_>>();
    segments.sort_by(|left, right| {
        left.start
            .y
            .total_cmp(&right.start.y)
            .then_with(|| left.start.x.total_cmp(&right.start.x))
    });
    LayerSlice {
        index,
        z,
        contours,
        segments,
        signed_twice_area,
        bounds,
    }
}

fn distance_squared(first: Vec2, second: Vec2) -> f64 {
    (first.x - second.x).powi(2) + (first.y - second.y).powi(2)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slicer::mesh::Vec3;

    fn cube() -> Mesh {
        let p = |x, y, z| Vec3 { x, y, z };
        let mut triangles = Vec::new();
        let faces = [
            [p(0., 0., 0.), p(1., 0., 0.), p(1., 1., 0.), p(0., 1., 0.)],
            [p(0., 0., 1.), p(0., 1., 1.), p(1., 1., 1.), p(1., 0., 1.)],
            [p(0., 0., 0.), p(0., 0., 1.), p(1., 0., 1.), p(1., 0., 0.)],
            [p(1., 0., 0.), p(1., 0., 1.), p(1., 1., 1.), p(1., 1., 0.)],
            [p(1., 1., 0.), p(1., 1., 1.), p(0., 1., 1.), p(0., 1., 0.)],
            [p(0., 1., 0.), p(0., 1., 1.), p(0., 0., 1.), p(0., 0., 0.)],
        ];
        for face in faces {
            triangles.push(Triangle([face[0], face[1], face[2]]));
            triangles.push(Triangle([face[0], face[2], face[3]]));
        }
        Mesh { triangles }
    }

    #[test]
    fn slices_cube_at_layer_midpoints() {
        let layers = slice_mesh(&cube(), 0.25).unwrap();
        assert_eq!(layers.len(), 4);
        for layer in layers {
            assert_eq!(layer.contours.len(), 1);
            assert!((layer.area() - 1.0).abs() < 1.0e-9);
            assert_eq!(layer.segments.len(), 8); // STL face diagonals remain as collinear edges.
        }
    }
}
