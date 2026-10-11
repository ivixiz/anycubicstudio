use std::collections::HashMap;

use rayon::prelude::*;
use smallvec::SmallVec;

use crate::{Error, Result};

use super::mesh::{Mesh, Triangle};

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

pub(crate) fn slice_mesh(
    mesh: &Mesh,
    layer_height: f64,
    stitch_tolerance_mm: f64,
    maximum_gap_mm: f64,
) -> Result<Vec<LayerSlice>> {
    slice_mesh_with_orientation(
        mesh,
        layer_height,
        stitch_tolerance_mm,
        maximum_gap_mm,
        ContourOrientation::Nested,
    )
}

/// Slices generated support primitives as a positive union. Supports are made
/// from overlapping closed cylinders/cones/branches and do not contain
/// intentional cavities. Treating a contour that happens to lie inside a
/// neighbouring primitive as a model hole creates layer-dependent pinholes at
/// branch joints.
pub(crate) fn slice_support_mesh(
    mesh: &Mesh,
    layer_height: f64,
    stitch_tolerance_mm: f64,
    maximum_gap_mm: f64,
) -> Result<Vec<LayerSlice>> {
    slice_mesh_with_orientation(
        mesh,
        layer_height,
        stitch_tolerance_mm,
        maximum_gap_mm,
        ContourOrientation::PositiveUnion,
    )
}

#[derive(Clone, Copy)]
enum ContourOrientation {
    Nested,
    PositiveUnion,
}

fn slice_mesh_with_orientation(
    mesh: &Mesh,
    layer_height: f64,
    stitch_tolerance_mm: f64,
    maximum_gap_mm: f64,
    orientation: ContourOrientation,
) -> Result<Vec<LayerSlice>> {
    if !layer_height.is_finite() || layer_height <= 0.0 {
        return Err(Error::InvalidModel(
            "layer height must be finite and greater than zero".into(),
        ));
    }
    if !stitch_tolerance_mm.is_finite() || stitch_tolerance_mm <= 0.0 {
        return Err(Error::InvalidModel(
            "contour stitch tolerance must be finite and positive".into(),
        ));
    }
    if !maximum_gap_mm.is_finite() || maximum_gap_mm < stitch_tolerance_mm {
        return Err(Error::InvalidModel(
            "maximum contour gap must be finite and at least the stitch tolerance".into(),
        ));
    }
    let (minimum, maximum) = mesh.bounds();
    if minimum.z < -stitch_tolerance_mm {
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

    super::parallel::install(|| {
        bins.into_par_iter()
            .enumerate()
            .map(|(index, candidates)| {
                let z = (index as f64 + 0.5) * layer_height;
                // A mathematical section exactly through a mesh vertex or a
                // coplanar triangle edge is ambiguous and can omit one incident
                // segment. Select the infinitesimally higher side consistently;
                // this is several orders below an LCD pixel and does not relax
                // contour validation.
                let section_z = z + stitch_tolerance_mm * 1.0e-4;
                let raw_segments = candidates
                    .into_iter()
                    .filter_map(|triangle| intersect_triangle(mesh.triangles[triangle], section_z))
                    .collect::<Vec<_>>();
                let mut contours =
                    stitch_segments(&raw_segments, stitch_tolerance_mm, maximum_gap_mm).map_err(
                        |message| {
                            Error::InvalidModel(format!("layer {index} at z={z:.6} mm: {message}"))
                        },
                    )?;
                match orientation {
                    ContourOrientation::Nested => orient_contours(&mut contours),
                    ContourOrientation::PositiveUnion => orient_positive(&mut contours),
                }
                Ok(prepare_layer(index as u32, z, contours))
            })
            .collect()
    })?
}

/// Combines independently oriented model/support contours. Keeping the two
/// winding classifications separate prevents a support inside the model's XY
/// projection from being mistaken for a hole.
pub(crate) fn merge_layers(
    model: Vec<LayerSlice>,
    supports: Vec<LayerSlice>,
    layer_height: f64,
) -> Result<Vec<LayerSlice>> {
    let count = model.len().max(supports.len());
    if model.is_empty() {
        return Ok(supports);
    }
    if supports.is_empty() {
        return Ok(model);
    }
    let mut model = model.into_iter();
    let mut supports = supports.into_iter();
    // Move polygon storage instead of retaining two complete copies of the
    // layer stream during merging. The encoded segments are rebuilt in place.
    let pairs = (0..count)
        .map(|i| (i, model.next(), supports.next()))
        .collect::<Vec<_>>();
    super::parallel::install(|| {
        pairs
            .into_par_iter()
            .map(|(index, model, support)| match (model, support) {
                (Some(mut model), Some(support)) => {
                    model.contours.extend(support.contours);
                    drop(model.segments);
                    drop(support.segments);
                    prepare_layer(
                        index as u32,
                        (index as f64 + 0.5) * layer_height,
                        model.contours,
                    )
                }
                (Some(layer), None) | (None, Some(layer)) => layer,
                (None, None) => unreachable!("layer count is the maximum input length"),
            })
            .collect()
    })
}

fn intersect_triangle(triangle: Triangle, z: f64) -> Option<[Vec2; 2]> {
    // A plane intersects a triangle in at most two unique edge crossings.
    // Keep the original arithmetic/order without a heap allocation per test.
    let mut intersections = [Vec2::default(); 3];
    let mut count = 0;
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
        if !intersections[..count]
            .iter()
            .any(|existing| distance_squared(*existing, point) < MIN_SEGMENT_LENGTH_SQUARED)
        {
            intersections[count] = point;
            count += 1;
        }
    }
    if count != 2
        || distance_squared(intersections[0], intersections[1]) < MIN_SEGMENT_LENGTH_SQUARED
    {
        None
    } else {
        Some([intersections[0], intersections[1]])
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct GridCell(i64, i64);

impl GridCell {
    fn new(point: Vec2, tolerance: f64) -> Self {
        Self(
            (point.x / tolerance).floor() as i64,
            (point.y / tolerance).floor() as i64,
        )
    }
}

fn stitch_segments(
    segments: &[[Vec2; 2]],
    tolerance: f64,
    maximum_gap: f64,
) -> std::result::Result<Vec<Vec<Vec2>>, String> {
    let tolerance_squared = tolerance * tolerance;
    let mut nodes = Vec::<Vec2>::with_capacity(segments.len());
    let mut cells = HashMap::<GridCell, SmallVec<[usize; 2]>>::with_capacity(segments.len());
    let mut endpoint_nodes = Vec::<[usize; 2]>::with_capacity(segments.len());
    for segment in segments {
        let mut endpoints = [0_usize; 2];
        for (endpoint_index, point) in segment.iter().copied().enumerate() {
            let cell = GridCell::new(point, tolerance);
            let mut nearest = None::<(usize, f64)>;
            for x_offset in -1..=1 {
                for y_offset in -1..=1 {
                    let neighbour = GridCell(cell.0 + x_offset, cell.1 + y_offset);
                    for node in cells.get(&neighbour).into_iter().flatten().copied() {
                        let distance = distance_squared(nodes[node], point);
                        if distance <= tolerance_squared
                            && nearest.is_none_or(|(_, best)| distance < best)
                        {
                            nearest = Some((node, distance));
                        }
                    }
                }
            }
            endpoints[endpoint_index] = if let Some((node, _)) = nearest {
                node
            } else {
                let node = nodes.len();
                nodes.push(point);
                cells.entry(cell).or_default().push(node);
                node
            };
        }
        endpoint_nodes.push(endpoints);
    }

    // CAD faces can be tessellated independently even when they share the
    // same topological edge. If the first resolution-sized clustering leaves
    // nearby odd-degree endpoint pairs within the caller's source-aware limit,
    // merge that pair explicitly. Larger holes remain fatal errors.
    let mut edges = parity_edges(&endpoint_nodes);
    if heal_odd_nodes(&mut nodes, &mut endpoint_nodes, &edges, maximum_gap) {
        edges = parity_edges(&endpoint_nodes);
    }
    let mut adjacency = vec![SmallVec::<[usize; 2]>::new(); nodes.len()];
    for (edge, [first, second]) in edges.iter().copied().enumerate() {
        adjacency[first].push(edge);
        adjacency[second].push(edge);
    }
    let odd_nodes = adjacency
        .iter()
        .enumerate()
        .filter_map(|(node, adjacent)| (adjacent.len() % 2 != 0).then_some(node))
        .collect::<Vec<_>>();
    if let Some(&node) = odd_nodes.first() {
        let nearest = odd_nodes
            .iter()
            .copied()
            .filter(|candidate| *candidate != node)
            .map(|candidate| {
                (
                    candidate,
                    distance_squared(nodes[node], nodes[candidate]).sqrt(),
                )
            })
            .min_by(|left, right| left.1.total_cmp(&right.1));
        let nearest_text = nearest.map_or_else(
            || "no other odd endpoint".to_owned(),
            |(candidate, distance)| {
                format!(
                    "nearest odd endpoint ({:.6}, {:.6}) is {:.6} mm away",
                    nodes[candidate].x, nodes[candidate].y, distance
                )
            },
        );
        return Err(format!(
            "open/non-manifold contour near ({:.6}, {:.6}); {} odd nodes, node degree {} after {:.6} mm welding; {}",
            nodes[node].x,
            nodes[node].y,
            odd_nodes.len(),
            adjacency[node].len(),
            tolerance,
            nearest_text
        ));
    }

    let mut used = vec![false; edges.len()];
    let mut contours = Vec::new();
    for first_edge in 0..edges.len() {
        if used[first_edge] {
            continue;
        }
        used[first_edge] = true;
        let start = edges[first_edge][0];
        let mut previous = start;
        let mut current = edges[first_edge][1];
        let mut contour = vec![nodes[start]];
        while current != start {
            contour.push(nodes[current]);
            let incoming = Vec2 {
                x: nodes[current].x - nodes[previous].x,
                y: nodes[current].y - nodes[previous].y,
            };
            let next_edge = adjacency[current]
                .iter()
                .copied()
                .filter(|edge| !used[*edge])
                .max_by(|left, right| {
                    continuation_score(incoming, current, edges[*left], &nodes).total_cmp(
                        &continuation_score(incoming, current, edges[*right], &nodes),
                    )
                })
                .ok_or_else(|| {
                    format!(
                        "open contour ends at ({:.6}, {:.6}) after {:.6} mm welding",
                        nodes[current].x, nodes[current].y, tolerance
                    )
                })?;
            used[next_edge] = true;
            let [first, second] = edges[next_edge];
            let next = if first == current { second } else { first };
            previous = current;
            current = next;
            if contour.len() > edges.len() + 1 {
                return Err("contour traversal did not terminate".into());
            }
        }
        remove_duplicate_neighbours(&mut contour, tolerance_squared);
        if contour.len() >= 3 && twice_area(&contour).abs() > 1.0e-12 {
            // These polygons live until ZIP/ACV export; release growth slack
            // once, instead of keeping up to 2x capacity across all layers.
            contour.shrink_to_fit();
            contours.push(contour);
        }
    }
    Ok(contours)
}

fn heal_odd_nodes(
    nodes: &mut [Vec2],
    endpoint_nodes: &mut [[usize; 2]],
    edges: &[[usize; 2]],
    maximum_distance: f64,
) -> bool {
    let mut degree = vec![0_usize; nodes.len()];
    for &[first, second] in edges {
        degree[first] += 1;
        degree[second] += 1;
    }
    let odd = degree
        .iter()
        .enumerate()
        .filter_map(|(node, degree)| (*degree % 2 != 0).then_some(node))
        .collect::<Vec<_>>();
    let limit_squared = maximum_distance * maximum_distance;
    let mut candidates = Vec::new();
    for (index, first) in odd.iter().copied().enumerate() {
        for second in odd.iter().copied().skip(index + 1) {
            let distance = distance_squared(nodes[first], nodes[second]);
            if distance <= limit_squared {
                candidates.push((distance, first, second));
            }
        }
    }
    // Globally shortest pairs win. Mutual-nearest matching is too strict for
    // a cluster of three or four independently tessellated CAD face seams.
    candidates.sort_by(|left, right| left.0.total_cmp(&right.0));
    let mut paired = vec![false; nodes.len()];
    let mut changed = false;
    for (_, node, candidate) in candidates {
        if paired[node] || paired[candidate] {
            continue;
        }
        paired[node] = true;
        paired[candidate] = true;
        changed = true;
        nodes[node] = Vec2 {
            x: (nodes[node].x + nodes[candidate].x) * 0.5,
            y: (nodes[node].y + nodes[candidate].y) * 0.5,
        };
        for endpoints in endpoint_nodes.iter_mut() {
            for endpoint in endpoints {
                if *endpoint == candidate {
                    *endpoint = node;
                }
            }
        }
    }
    changed
}

/// Coincident triangle/shell faces contribute the same intersection segment
/// twice. Contour boundaries are an XOR: an even multiplicity is internal and
/// disappears, while an odd multiplicity contributes one edge.
fn parity_edges(endpoint_nodes: &[[usize; 2]]) -> Vec<[usize; 2]> {
    let mut positions = HashMap::<(usize, usize), usize>::with_capacity(endpoint_nodes.len());
    let mut odd = Vec::<Option<[usize; 2]>>::with_capacity(endpoint_nodes.len());
    for [first, second] in endpoint_nodes.iter().copied() {
        if first == second {
            continue;
        }
        let key = if first < second {
            (first, second)
        } else {
            (second, first)
        };
        match positions.entry(key) {
            std::collections::hash_map::Entry::Vacant(entry) => {
                entry.insert(odd.len());
                odd.push(Some([first, second]));
            }
            std::collections::hash_map::Entry::Occupied(entry) => {
                let edge = &mut odd[*entry.get()];
                *edge = if edge.is_some() {
                    None
                } else {
                    Some([first, second])
                };
            }
        }
    }
    // Stable source order keeps contour traversal and vertex deduplication
    // independent of hash seeds and worker scheduling.
    odd.into_iter().flatten().collect()
}

fn continuation_score(incoming: Vec2, current: usize, edge: [usize; 2], nodes: &[Vec2]) -> f64 {
    let next = if edge[0] == current { edge[1] } else { edge[0] };
    let outgoing = Vec2 {
        x: nodes[next].x - nodes[current].x,
        y: nodes[next].y - nodes[current].y,
    };
    let denominator = (incoming.x * incoming.x + incoming.y * incoming.y).sqrt()
        * (outgoing.x * outgoing.x + outgoing.y * outgoing.y).sqrt();
    if denominator == 0.0 {
        -1.0
    } else {
        (incoming.x * outgoing.x + incoming.y * outgoing.y) / denominator
    }
}

fn remove_duplicate_neighbours(contour: &mut Vec<Vec2>, tolerance_squared: f64) {
    contour.dedup_by(|left, right| distance_squared(*left, *right) <= tolerance_squared);
    if contour.len() > 1
        && distance_squared(contour[0], *contour.last().unwrap()) <= tolerance_squared
    {
        contour.pop();
    }
}

fn orient_contours(contours: &mut [Vec<Vec2>]) {
    let bounds = contours
        .iter()
        .map(|contour| {
            contour.iter().fold(
                [
                    f64::INFINITY,
                    f64::INFINITY,
                    f64::NEG_INFINITY,
                    f64::NEG_INFINITY,
                ],
                |b, p| [b[0].min(p.x), b[1].min(p.y), b[2].max(p.x), b[3].max(p.y)],
            )
        })
        .collect::<Vec<_>>();
    let winding = contours
        .iter()
        .enumerate()
        .map(|(index, contour)| {
            let probe = contour[0];
            let depth = contours
                .iter()
                .enumerate()
                .filter(|(other, polygon)| {
                    let b = bounds[*other];
                    *other != index
                        && probe.x >= b[0]
                        && probe.x <= b[2]
                        && probe.y >= b[1]
                        && probe.y <= b[3]
                        && point_in_polygon(probe, polygon)
                })
                .count();
            depth % 2 == 0
        })
        .collect::<Vec<_>>();
    for (contour, should_be_counter_clockwise) in contours.iter_mut().zip(winding) {
        if (twice_area(contour) > 0.0) != should_be_counter_clockwise {
            contour.reverse();
        }
    }
}

fn orient_positive(contours: &mut [Vec<Vec2>]) {
    for contour in contours {
        if twice_area(contour) < 0.0 {
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

pub(crate) fn layer_from_contours(index: u32, z: f64, contours: Vec<Vec<Vec2>>) -> LayerSlice {
    prepare_layer(index, z, contours)
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

    fn scaled_translated_cube(scale: f64, offset: Vec3) -> Mesh {
        let mut result = cube();
        for triangle in &mut result.triangles {
            for point in &mut triangle.0 {
                point.x = point.x * scale + offset.x;
                point.y = point.y * scale + offset.y;
                point.z = point.z * scale + offset.z;
            }
        }
        result
    }

    #[test]
    fn slices_cube_at_layer_midpoints() {
        let layers = slice_mesh(&cube(), 0.25, 1.0e-5, 5.0e-5).unwrap();
        assert_eq!(layers.len(), 4);
        for layer in layers {
            assert_eq!(layer.contours.len(), 1);
            assert!((layer.area() - 1.0).abs() < 1.0e-9);
            assert_eq!(layer.segments.len(), 8); // STL face diagonals remain as collinear edges.
        }
    }

    #[test]
    fn nested_support_solids_remain_positive_instead_of_becoming_holes() {
        let mut supports = scaled_translated_cube(2.0, Vec3::default());
        supports.triangles.extend(
            scaled_translated_cube(
                1.0,
                Vec3 {
                    x: 0.5,
                    y: 0.5,
                    z: 0.0,
                },
            )
            .triangles,
        );

        let model_layer = &slice_mesh(&supports, 0.25, 1.0e-5, 5.0e-5).unwrap()[0];
        assert_eq!(model_layer.contours.len(), 2);
        assert!((model_layer.area() - 3.0).abs() < 1.0e-9);

        let support_layer = &slice_support_mesh(&supports, 0.25, 1.0e-5, 5.0e-5).unwrap()[0];
        assert_eq!(support_layer.contours.len(), 2);
        assert!(
            support_layer
                .contours
                .iter()
                .all(|contour| twice_area(contour) > 0.0)
        );
        assert!((support_layer.area() - 5.0).abs() < 1.0e-9);
    }

    #[test]
    fn stitches_sub_resolution_cracks_but_rejects_real_open_contours() {
        let almost_closed = [
            [Vec2 { x: 0.0, y: 0.0 }, Vec2 { x: 1.0, y: 0.0 }],
            [Vec2 { x: 1.001, y: 0.0 }, Vec2 { x: 1.0, y: 1.0 }],
            [Vec2 { x: 1.0, y: 1.0 }, Vec2 { x: 0.0, y: 1.0 }],
            [Vec2 { x: 0.0, y: 1.0 }, Vec2 { x: 0.0, y: 0.0 }],
        ];
        assert_eq!(
            stitch_segments(&almost_closed, 0.002, 0.01).unwrap().len(),
            1
        );
        assert!(stitch_segments(&almost_closed, 0.0001, 0.0005).is_err());
    }

    #[test]
    fn heals_an_exporter_crack_only_inside_the_explicit_gap_limit() {
        let exporter_crack = [
            [Vec2 { x: 0.0, y: 0.0 }, Vec2 { x: 1.0, y: 0.0 }],
            [Vec2 { x: 1.086, y: 0.0 }, Vec2 { x: 1.0, y: 1.0 }],
            [Vec2 { x: 1.0, y: 1.0 }, Vec2 { x: 0.0, y: 1.0 }],
            [Vec2 { x: 0.0, y: 1.0 }, Vec2 { x: 0.0, y: 0.0 }],
        ];
        let lcd_pixel = 0.0168;
        assert!(stitch_segments(&exporter_crack, lcd_pixel, lcd_pixel * 5.0).is_err());
        assert_eq!(
            stitch_segments(&exporter_crack, lcd_pixel, lcd_pixel * 8.0)
                .unwrap()
                .len(),
            1
        );
    }
}
