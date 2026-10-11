use std::{
    cmp::Reverse,
    collections::{BinaryHeap, HashMap},
};

use crate::{Error, Result};

use super::{
    SupportConfig, SupportPoint, SupportShape,
    mesh::{Mesh, Triangle, Vec3},
};

#[derive(Debug, Clone)]
pub(crate) struct GeneratedSupports {
    pub mesh: Mesh,
    pub count: usize,
}

#[derive(Debug, Clone, Copy)]
struct SupportAnchor {
    contact: Vec3,
    base_z: f64,
    automatic: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct GridKey(i64, i64);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct ContactGridKey(i64, i64, i64);

#[derive(Debug, Clone)]
struct CandidateContact {
    point: SupportPoint,
    automatic: bool,
}

struct CollisionIndex<'a> {
    tree: super::spatial::TriangleIndex<'a>,
}

/// Conservative voxel occupancy used only as a fast rejection stage. Every
/// accepted segment is still checked against exact triangle distances by
/// `CollisionIndex`; this grid prevents thousands of obviously blocked
/// visibility candidates from repeatedly walking the dense triangle index.
struct RoutingGrid {
    step: f64,
    half_x: f64,
    half_y: f64,
    size_x: usize,
    size_y: usize,
    size_z: usize,
    occupied: Vec<bool>,
}

impl RoutingGrid {
    fn new(model: &Mesh, build_volume_mm: [f64; 3], radius: f64, spacing: f64) -> Self {
        let mut step = (spacing * 0.25).clamp(1.0, 2.0);
        let estimated_cells = build_volume_mm.iter().product::<f64>() / step.powi(3);
        const MAXIMUM_CELLS: f64 = 8_000_000.0;
        if estimated_cells > MAXIMUM_CELLS {
            step *= (estimated_cells / MAXIMUM_CELLS).cbrt();
        }
        let half_x = build_volume_mm[0] * 0.5;
        let half_y = build_volume_mm[1] * 0.5;
        let size_x = (build_volume_mm[0] / step).ceil() as usize;
        let size_y = (build_volume_mm[1] / step).ceil() as usize;
        let size_z = (build_volume_mm[2] / step).ceil() as usize;
        let mut grid = Self {
            step,
            half_x,
            half_y,
            size_x,
            size_y,
            size_z,
            occupied: vec![false; size_x.saturating_mul(size_y).saturating_mul(size_z)],
        };
        for triangle in &model.triangles {
            let min_x = triangle
                .0
                .iter()
                .map(|point| point.x)
                .fold(f64::INFINITY, f64::min)
                - radius;
            let max_x = triangle
                .0
                .iter()
                .map(|point| point.x)
                .fold(f64::NEG_INFINITY, f64::max)
                + radius;
            let min_y = triangle
                .0
                .iter()
                .map(|point| point.y)
                .fold(f64::INFINITY, f64::min)
                - radius;
            let max_y = triangle
                .0
                .iter()
                .map(|point| point.y)
                .fold(f64::NEG_INFINITY, f64::max)
                + radius;
            let min_z = triangle
                .0
                .iter()
                .map(|point| point.z)
                .fold(f64::INFINITY, f64::min)
                - radius;
            let max_z = triangle
                .0
                .iter()
                .map(|point| point.z)
                .fold(f64::NEG_INFINITY, f64::max)
                + radius;
            let Some((first_x, last_x)) = grid.index_range(min_x, max_x, -half_x, size_x) else {
                continue;
            };
            let Some((first_y, last_y)) = grid.index_range(min_y, max_y, -half_y, size_y) else {
                continue;
            };
            let Some((first_z, last_z)) = grid.index_range(min_z, max_z, 0.0, size_z) else {
                continue;
            };
            let voxel_reach = radius + step * 3.0_f64.sqrt() * 0.5;
            let voxel_reach_squared = voxel_reach * voxel_reach;
            for z in first_z..=last_z {
                for y in first_y..=last_y {
                    let row = (z * size_y + y) * size_x;
                    for x in first_x..=last_x {
                        if grid.occupied[row + x] {
                            continue;
                        }
                        let center = Vec3 {
                            x: -half_x + (x as f64 + 0.5) * step,
                            y: -half_y + (y as f64 + 0.5) * step,
                            z: (z as f64 + 0.5) * step,
                        };
                        if point_triangle_distance_squared(center, *triangle) <= voxel_reach_squared
                        {
                            grid.occupied[row + x] = true;
                        }
                    }
                }
            }
        }
        grid
    }

    fn index_range(
        &self,
        minimum: f64,
        maximum: f64,
        origin: f64,
        size: usize,
    ) -> Option<(usize, usize)> {
        if size == 0 || maximum < origin || minimum > origin + size as f64 * self.step {
            return None;
        }
        let first = ((minimum - origin) / self.step).floor().max(0.0) as usize;
        let last = ((maximum - origin) / self.step)
            .floor()
            .clamp(0.0, size.saturating_sub(1) as f64) as usize;
        Some((first.min(size - 1), last))
    }

    fn point_occupied(&self, point: Vec3) -> bool {
        let x = ((point.x + self.half_x) / self.step).floor() as isize;
        let y = ((point.y + self.half_y) / self.step).floor() as isize;
        let z = (point.z / self.step).floor() as isize;
        if x < 0
            || y < 0
            || z < 0
            || x as usize >= self.size_x
            || y as usize >= self.size_y
            || z as usize >= self.size_z
        {
            return true;
        }
        self.occupied[(z as usize * self.size_y + y as usize) * self.size_x + x as usize]
    }

    fn path_maybe_clear(&self, path: &[Vec3]) -> bool {
        path.windows(2).all(|segment| {
            let length = distance(segment[0], segment[1]);
            let steps = (length / (self.step * 0.5)).ceil().max(1.0) as usize;
            // The final sample is excluded because a support shoulder is
            // intentionally close to its target surface. Exact capsule
            // checking below still validates that final approach.
            let mut occupied_run = 0;
            (0..steps).all(|index| {
                let fraction = index as f64 / steps as f64;
                let occupied = self.point_occupied(Vec3 {
                    x: segment[0].x + (segment[1].x - segment[0].x) * fraction,
                    y: segment[0].y + (segment[1].y - segment[0].y) * fraction,
                    z: segment[0].z + (segment[1].z - segment[0].z) * fraction,
                });
                if occupied {
                    occupied_run += 1;
                } else {
                    occupied_run = 0;
                }
                occupied_run < 3
            })
        })
    }

    fn planar_path(&self, start: Vec3, end: Vec3, maximum_expansions: usize) -> Option<Vec<Vec3>> {
        let start_cell = self.planar_cell(start)?;
        let end_cell = self.planar_cell(end)?;
        let z = (start.z / self.step).floor() as usize;
        if z >= self.size_z {
            return None;
        }
        let cell_count = self.size_x.checked_mul(self.size_y)?;
        let start_id = start_cell.1 * self.size_x + start_cell.0;
        let end_id = end_cell.1 * self.size_x + end_cell.0;
        let mut scores = vec![usize::MAX; cell_count];
        let mut parents = vec![usize::MAX; cell_count];
        let mut open = BinaryHeap::<Reverse<(usize, usize)>>::new();
        scores[start_id] = 0;
        open.push(Reverse((planar_heuristic(start_cell, end_cell), start_id)));
        let mut expansions = 0;
        while let Some(Reverse((estimated, id))) = open.pop() {
            let x = id % self.size_x;
            let y = id / self.size_x;
            let score = scores[id];
            if score == usize::MAX
                || estimated > score.saturating_add(planar_heuristic((x, y), end_cell))
            {
                continue;
            }
            if id == end_id {
                let mut ids = vec![id];
                let mut cursor = id;
                while cursor != start_id {
                    cursor = parents[cursor];
                    if cursor == usize::MAX {
                        return None;
                    }
                    ids.push(cursor);
                }
                ids.reverse();
                let mut points = Vec::new();
                points.push(start);
                for cells in ids.windows(3) {
                    let first = (cells[0] % self.size_x, cells[0] / self.size_x);
                    let middle = (cells[1] % self.size_x, cells[1] / self.size_x);
                    let last = (cells[2] % self.size_x, cells[2] / self.size_x);
                    let before = (
                        middle.0 as isize - first.0 as isize,
                        middle.1 as isize - first.1 as isize,
                    );
                    let after = (
                        last.0 as isize - middle.0 as isize,
                        last.1 as isize - middle.1 as isize,
                    );
                    if before != after {
                        points.push(Vec3 {
                            x: -self.half_x + (middle.0 as f64 + 0.5) * self.step,
                            y: -self.half_y + (middle.1 as f64 + 0.5) * self.step,
                            z: start.z,
                        });
                    }
                }
                points.push(end);
                return Some(compact_path(points));
            }
            expansions += 1;
            if expansions > maximum_expansions {
                return None;
            }
            for y_offset in -1_isize..=1 {
                for x_offset in -1_isize..=1 {
                    if x_offset == 0 && y_offset == 0 {
                        continue;
                    }
                    let next_x = x as isize + x_offset;
                    let next_y = y as isize + y_offset;
                    if next_x < 0
                        || next_y < 0
                        || next_x as usize >= self.size_x
                        || next_y as usize >= self.size_y
                    {
                        continue;
                    }
                    let next_x = next_x as usize;
                    let next_y = next_y as usize;
                    let next_id = next_y * self.size_x + next_x;
                    if next_id != end_id
                        && self.occupied[(z * self.size_y + next_y) * self.size_x + next_x]
                    {
                        continue;
                    }
                    let movement = if x_offset == 0 || y_offset == 0 {
                        10
                    } else {
                        14
                    };
                    let next_score = score.saturating_add(movement);
                    if next_score < scores[next_id] {
                        scores[next_id] = next_score;
                        parents[next_id] = id;
                        open.push(Reverse((
                            next_score.saturating_add(planar_heuristic((next_x, next_y), end_cell)),
                            next_id,
                        )));
                    }
                }
            }
        }
        None
    }

    fn planar_cell(&self, point: Vec3) -> Option<(usize, usize)> {
        let x = ((point.x + self.half_x) / self.step).floor() as isize;
        let y = ((point.y + self.half_y) / self.step).floor() as isize;
        (x >= 0 && y >= 0 && (x as usize) < self.size_x && (y as usize) < self.size_y)
            .then_some((x as usize, y as usize))
    }
}

fn planar_heuristic(first: (usize, usize), second: (usize, usize)) -> usize {
    let dx = first.0.abs_diff(second.0);
    let dy = first.1.abs_diff(second.1);
    let diagonal = dx.min(dy);
    diagonal * 14 + (dx.max(dy) - diagonal) * 10
}

impl<'a> CollisionIndex<'a> {
    fn new(model: &'a Mesh) -> Self {
        Self {
            tree: super::spatial::TriangleIndex::new(model),
        }
    }

    fn capsule_clear(&self, start: Vec3, end: Vec3, radius: f64) -> bool {
        let radius = radius.max(0.0);
        let radius_squared = radius * radius;
        !self.tree.any_hit(start, end, radius, |triangle| {
            segment_triangle_distance_squared(start, end, triangle) <= radius_squared
        })
    }
}

pub(crate) fn generate_supports(
    model: &Mesh,
    config: &SupportConfig,
    build_volume_mm: [f64; 3],
    layer_height_mm: f64,
) -> Result<GeneratedSupports> {
    if !config.enabled {
        return Ok(GeneratedSupports {
            mesh: Mesh {
                triangles: Vec::new(),
            },
            count: 0,
        });
    }

    let mut timings = super::timing::Timings::new("supports");
    let mut contacts = config
        .manual_points
        .iter()
        .cloned()
        .map(|point| CandidateContact {
            point,
            automatic: false,
        })
        .collect::<Vec<_>>();
    if config.auto_generate {
        contacts.extend(
            auto_contacts(model, config)
                .into_iter()
                .map(|point| CandidateContact {
                    point,
                    automatic: true,
                }),
        );
    }
    let sampled_contact_count = contacts.len();
    timings.lap("sample contacts");
    contacts.sort_by(|left, right| {
        left.point.contact_mm[2]
            .total_cmp(&right.point.contact_mm[2])
            .then_with(|| left.point.contact_mm[0].total_cmp(&right.point.contact_mm[0]))
            .then_with(|| left.point.contact_mm[1].total_cmp(&right.point.contact_mm[1]))
    });
    let support_clearance = (config.tip_diameter_mm * 0.5).max(0.3);
    let contact_spacing = effective_contact_spacing(config);
    let minimum_xy_distance = if config.shape == SupportShape::Branch {
        (contact_spacing * 0.7).max(config.tip_diameter_mm * 1.5)
    } else {
        contact_spacing.max(config.base_diameter_mm + support_clearance)
    };
    contacts = separate_contacts(
        contacts,
        minimum_xy_distance,
        (config.shape == SupportShape::Branch).then_some(
            (config.tip_height_mm * 1.5)
                .max(config.trunk_diameter_mm)
                .max(1.0),
        ),
    );
    let separated_contact_count = contacts.len();

    let needs_surface_query = contacts.iter().any(|contact| {
        (contact.automatic && config.shape != SupportShape::Branch)
            || (!contact.automatic && contact.point.base_z_mm.is_none())
    });
    let surface_index = needs_surface_query.then(|| super::spatial::TriangleIndex::new(model));
    let minimum_z = if needs_surface_query {
        model.bounds().0.z
    } else {
        0.0
    };

    let mut triangles = Vec::new();
    let mut retained = Vec::<SupportAnchor>::new();
    for support in contacts {
        let point = support.point;
        let contact = Vec3 {
            x: point.contact_mm[0],
            y: point.contact_mm[1],
            z: point.contact_mm[2],
        };
        if !contact.x.is_finite() || !contact.y.is_finite() || !contact.z.is_finite() {
            return Err(Error::InvalidModel(
                "support contact contains a non-finite coordinate".into(),
            ));
        }
        // A branch contact is routed against the spatial collision index later;
        // scanning every triangle here as well made dense meshes quadratic.
        let obstructed = surface_index.as_ref().and_then(|index| {
            highest_surface_below(index, minimum_z, contact.x, contact.y, contact.z)
        });
        let base_z = if support.automatic {
            if config.shape != SupportShape::Branch && obstructed.is_some() {
                continue;
            }
            0.0
        } else {
            point.base_z_mm.unwrap_or_else(|| obstructed.unwrap_or(0.0))
        };
        if base_z < -1.0e-6
            || contact.z - base_z < config.min_height_mm
            || contact.z - base_z > config.max_height_mm
        {
            continue;
        }
        let base_z = base_z.max(0.0);
        if config.shape != SupportShape::Branch {
            append_pillar(&mut triangles, contact, base_z, config);
        }
        retained.push(SupportAnchor {
            contact,
            base_z,
            automatic: support.automatic,
        });
    }
    timings.lap("filter contacts");
    let count = if config.shape == SupportShape::Branch {
        append_branch_forest(
            &mut triangles,
            model,
            &retained,
            config,
            build_volume_mm,
            layer_height_mm,
        )
    } else {
        retained.len()
    };
    timings.lap("route and build");
    if std::env::var_os("ANYCUBIC_SUPPORT_DIAGNOSTICS").is_some() {
        eprintln!(
            "Support routing: {sampled_contact_count} sampled, {separated_contact_count} separated, {} height-eligible, {count} routed",
            retained.len()
        );
    }

    Ok(GeneratedSupports {
        count,
        mesh: Mesh { triangles },
    })
}

fn separate_contacts(
    contacts: Vec<CandidateContact>,
    minimum_distance: f64,
    vertical_separation: Option<f64>,
) -> Vec<CandidateContact> {
    let mut retained = Vec::<CandidateContact>::new();
    let mut cells = HashMap::<GridKey, Vec<usize>>::new();
    let minimum_squared = minimum_distance * minimum_distance;
    for contact in contacts {
        let x = contact.point.contact_mm[0];
        let y = contact.point.contact_mm[1];
        let z = contact.point.contact_mm[2];
        let cell = GridKey(
            (x / minimum_distance).floor() as i64,
            (y / minimum_distance).floor() as i64,
        );
        let collides = (-1..=1).any(|x_offset| {
            (-1..=1).any(|y_offset| {
                cells
                    .get(&GridKey(cell.0 + x_offset, cell.1 + y_offset))
                    .into_iter()
                    .flatten()
                    .any(|index| {
                        let other = &retained[*index];
                        let xy_collision = (x - other.point.contact_mm[0]).powi(2)
                            + (y - other.point.contact_mm[1]).powi(2)
                            < minimum_squared;
                        let z_collision = vertical_separation.is_none_or(|minimum_z| {
                            (z - other.point.contact_mm[2]).abs() < minimum_z
                        });
                        xy_collision && z_collision
                    })
            })
        });
        if !collides {
            cells.entry(cell).or_default().push(retained.len());
            retained.push(contact);
        }
    }
    retained
}

#[derive(Debug)]
struct TreeRoute {
    root: Vec3,
    paths: Vec<Vec<Vec3>>,
    contact_indices: Vec<usize>,
}

/// Builds deterministic collision-aware trees. Contacts first try a trunk
/// directly below their cluster. When model geometry blocks that trunk (a
/// common case when a raft/base is part of the imported mesh), roots are
/// projected outside the model's XY bounds and arms approach from the side.
/// This follows the routing principles of an SLA tree builder without claiming
/// topology parity with Workshop's proprietary VoxSDK implementation.
fn append_branch_forest(
    output: &mut Vec<Triangle>,
    model: &Mesh,
    supports: &[SupportAnchor],
    config: &SupportConfig,
    build_volume_mm: [f64; 3],
    layer_height_mm: f64,
) -> usize {
    let mut count = 0;
    // Explicit/manual contacts retain their requested base semantics. They are
    // not silently moved to a different point on the build plate.
    for support in supports.iter().filter(|support| !support.automatic) {
        append_pillar(output, support.contact, support.base_z, config);
        count += 1;
    }

    let contact_spacing = effective_contact_spacing(config);
    let mut timings = super::timing::Timings::new("trees");
    let mut collision = CollisionIndex::new(model);
    timings.lap("collision index");
    let root_pool = build_tree_root_pool(&mut collision, model, config, build_volume_mm);
    timings.lap("root pool");
    if root_pool.is_empty() {
        return count;
    }
    let routing_clearance = (config.tip_diameter_mm * 0.5).max(0.3);
    let routing_radius =
        (config.trunk_diameter_mm * 0.5).max(config.tip_diameter_mm * 0.5) + routing_clearance;
    let routing_grid = RoutingGrid::new(model, build_volume_mm, routing_radius, contact_spacing);
    timings.lap("routing grid");
    let mut used_roots = Vec::<Vec3>::new();
    let mut assigned = vec![false; supports.len()];
    let branch_radius = config
        .branch_max_distance_mm
        .min((contact_spacing * 3.0).max(config.base_diameter_mm * 1.5));
    let branch_radius_squared = branch_radius * branch_radius;
    for seed in 0..supports.len() {
        if assigned[seed] || !supports[seed].automatic {
            continue;
        }
        let origin = supports[seed];
        let mut candidates = supports
            .iter()
            .enumerate()
            .filter(|(index, support)| {
                !assigned[*index]
                    && support.automatic
                    && (support.contact.z - origin.contact.z).abs()
                        <= (contact_spacing * 2.0).max(config.tip_height_mm * 3.0)
                    && (support.contact.x - origin.contact.x).powi(2)
                        + (support.contact.y - origin.contact.y).powi(2)
                        <= branch_radius_squared
            })
            .map(|(index, support)| {
                (
                    (support.contact.x - origin.contact.x).powi(2)
                        + (support.contact.y - origin.contact.y).powi(2),
                    index,
                )
            })
            .collect::<Vec<_>>();
        candidates.sort_by(|left, right| {
            left.0
                .total_cmp(&right.0)
                .then_with(|| left.1.cmp(&right.1))
        });
        let group = candidates
            .into_iter()
            .take(config.branch_max_children.max(1) as usize)
            .map(|(_, index)| index)
            .collect::<Vec<_>>();

        if let Some(route) = find_tree_route(
            &group,
            supports,
            config,
            &root_pool,
            build_volume_mm,
            &used_roots,
            &mut collision,
            &routing_grid,
            layer_height_mm,
        ) {
            for index in &route.contact_indices {
                assigned[*index] = true;
            }
            emit_tree_route(output, &route, supports, config);
            used_roots.push(route.root);
            count += route.contact_indices.len();
            continue;
        }
        // No legal angle-limited route exists for the seed. Do not generate a
        // detached support or an unbounded tour around the model.
        assigned[seed] = true;
    }
    timings.lap("routes");
    count
}

#[allow(clippy::too_many_arguments)]
fn find_tree_route(
    group: &[usize],
    supports: &[SupportAnchor],
    config: &SupportConfig,
    root_pool: &[Vec3],
    build_volume_mm: [f64; 3],
    used_roots: &[Vec3],
    collision: &mut CollisionIndex<'_>,
    routing_grid: &RoutingGrid,
    layer_height_mm: f64,
) -> Option<TreeRoute> {
    let inverse = 1.0 / group.len() as f64;
    let center = Vec3 {
        x: group
            .iter()
            .map(|index| supports[*index].contact.x)
            .sum::<f64>()
            * inverse,
        y: group
            .iter()
            .map(|index| supports[*index].contact.y)
            .sum::<f64>()
            * inverse,
        z: 0.0,
    };
    let shoulders = group
        .iter()
        .map(|index| {
            let contact = supports[*index].contact;
            (
                *index,
                Vec3 {
                    x: contact.x,
                    y: contact.y,
                    z: contact.z - config.tip_height_mm,
                },
            )
        })
        .collect::<Vec<_>>();
    let trunk_floor = config.base_height_mm + config.trunk_diameter_mm;
    if shoulders
        .iter()
        .any(|(_, shoulder)| shoulder.z <= trunk_floor + 1.0e-4)
    {
        return None;
    }

    let clearance = (config.tip_diameter_mm * 0.5).max(0.3);
    let base_radius = config.base_diameter_mm * 0.5;
    let root_spacing = config.base_diameter_mm + clearance;
    let mut roots = Vec::with_capacity(root_pool.len() + 1);
    roots.push(center);
    roots.extend_from_slice(root_pool);
    roots.sort_by(|left, right| {
        let left_distance = (left.x - center.x).powi(2) + (left.y - center.y).powi(2);
        let right_distance = (right.x - center.x).powi(2) + (right.y - center.y).powi(2);
        left_distance
            .total_cmp(&right_distance)
            .then_with(|| left.x.total_cmp(&right.x))
            .then_with(|| left.y.total_cmp(&right.y))
    });
    roots.dedup_by(|left, right| {
        (left.x - right.x).abs() < 1.0e-6 && (left.y - right.y).abs() < 1.0e-6
    });
    let trunk_collision_radius = config.branch_max_trunk_diameter_mm * 0.5 + clearance;
    let branch_collision_radius =
        (config.trunk_diameter_mm * 0.5).max(config.tip_diameter_mm * 0.5) + clearance;

    for (root_index, root) in roots
        .into_iter()
        .filter(|root| {
            !used_roots.iter().any(|other| {
                (root.x - other.x).powi(2) + (root.y - other.y).powi(2)
                    < root_spacing * root_spacing
            })
        })
        .take(64)
        .enumerate()
    {
        if root.x - base_radius < -build_volume_mm[0] * 0.5
            || root.x + base_radius > build_volume_mm[0] * 0.5
            || root.y - base_radius < -build_volume_mm[1] * 0.5
            || root.y + base_radius > build_volume_mm[1] * 0.5
        {
            continue;
        }
        let base_top = Vec3 {
            z: config.base_height_mm,
            ..root
        };
        if !collision.capsule_clear(root, base_top, base_radius + clearance) {
            continue;
        }
        let mut paths = Vec::new();
        let mut contact_indices = Vec::new();
        for (candidate_index, (contact_index, shoulder)) in shoulders.iter().enumerate() {
            let path = find_branch_path(
                root,
                base_top,
                *shoulder,
                trunk_floor,
                trunk_collision_radius,
                branch_collision_radius,
                config,
                build_volume_mm,
                collision,
                routing_grid,
                root_index < 6,
                layer_height_mm,
            );
            if let Some(path) = path {
                paths.push(path);
                contact_indices.push(*contact_index);
            } else if candidate_index == 0 {
                // The seed contact defines this tree. Try another root rather
                // than creating a tree containing only unrelated neighbours.
                paths.clear();
                break;
            }
            if paths.len() >= config.branch_max_children as usize {
                break;
            }
        }
        if !paths.is_empty() {
            return Some(TreeRoute {
                root,
                paths,
                contact_indices,
            });
        }
    }
    None
}

#[allow(clippy::too_many_arguments)]
fn find_branch_path(
    root: Vec3,
    base_top: Vec3,
    shoulder: Vec3,
    trunk_floor: f64,
    trunk_radius: f64,
    branch_radius: f64,
    config: &SupportConfig,
    build_volume_mm: [f64; 3],
    collision: &mut CollisionIndex<'_>,
    routing_grid: &RoutingGrid,
    allow_planar_search: bool,
    layer_height_mm: f64,
) -> Option<Vec<Vec3>> {
    let available = shoulder.z - trunk_floor;
    if available <= 1.0e-4 {
        return None;
    }
    // Even the straight XY route needs this much vertical drop. No dogleg or
    // A* detour can shorten it; reject before allocating/searching any paths.
    let direct = (shoulder.x - root.x).hypot(shoulder.y - root.y);
    if direct / config.overhang_angle_deg.to_radians().tan() > available + 1.0e-7 {
        return None;
    }
    let spacing = effective_contact_spacing(config);
    let mut skeletons = vec![vec![root, shoulder]];
    skeletons.push(vec![
        root,
        Vec3 {
            x: shoulder.x,
            y: root.y,
            z: 0.0,
        },
        shoulder,
    ]);
    skeletons.push(vec![
        root,
        Vec3 {
            x: root.x,
            y: shoulder.y,
            z: 0.0,
        },
        shoulder,
    ]);

    let delta_x = shoulder.x - root.x;
    let delta_y = shoulder.y - root.y;
    let direct_reach = delta_x.hypot(delta_y);
    if direct_reach > 1.0e-9 {
        let normal_x = -delta_y / direct_reach;
        let normal_y = delta_x / direct_reach;
        for multiplier in [0.5, -0.5, 1.0, -1.0] {
            let offset = (spacing * multiplier).clamp(
                -config.branch_max_distance_mm,
                config.branch_max_distance_mm,
            );
            skeletons.push(vec![
                root,
                Vec3 {
                    x: (root.x + shoulder.x) * 0.5 + normal_x * offset,
                    y: (root.y + shoulder.y) * 0.5 + normal_y * offset,
                    z: 0.0,
                },
                shoulder,
            ]);
        }
    }

    for skeleton in skeletons {
        let Some(path) =
            grade_branch_skeleton(skeleton, shoulder, trunk_floor, config, layer_height_mm)
        else {
            continue;
        };
        let start = path[0];
        if !collision.capsule_clear(base_top, start, trunk_radius) {
            continue;
        }
        if branch_path_is_clear(
            &path,
            branch_radius,
            build_volume_mm,
            collision,
            routing_grid,
            config.overhang_angle_deg,
        ) {
            return Some(path);
        }
    }

    // A* only supplies an XY skeleton. Re-grading it over Z is mandatory:
    // accepting its original planar Z values was the source of 90° supports.
    if allow_planar_search {
        let probe_z = trunk_floor + available * 0.5;
        let probe_start = Vec3 {
            x: root.x,
            y: root.y,
            z: probe_z,
        };
        let probe_end = Vec3 {
            x: shoulder.x,
            y: shoulder.y,
            z: probe_z,
        };
        if let Some(mut skeleton) = routing_grid.planar_path(probe_start, probe_end, 8_000) {
            if let Some(first) = skeleton.first_mut() {
                *first = root;
            }
            if let Some(last) = skeleton.last_mut() {
                *last = shoulder;
            }
            if let Some(path) =
                grade_branch_skeleton(skeleton, shoulder, trunk_floor, config, layer_height_mm)
            {
                let start = path[0];
                if collision.capsule_clear(base_top, start, trunk_radius)
                    && branch_path_is_clear(
                        &path,
                        branch_radius,
                        build_volume_mm,
                        collision,
                        routing_grid,
                        config.overhang_angle_deg,
                    )
                {
                    return Some(path);
                }
            }
        }
    }
    None
}

fn grade_branch_skeleton(
    skeleton: Vec<Vec3>,
    shoulder: Vec3,
    trunk_floor: f64,
    config: &SupportConfig,
    layer_height_mm: f64,
) -> Option<Vec<Vec3>> {
    let skeleton = compact_path(skeleton);
    if skeleton.len() < 2 {
        return None;
    }
    let horizontal_lengths = skeleton
        .windows(2)
        .map(|segment| (segment[1].x - segment[0].x).hypot(segment[1].y - segment[0].y))
        .collect::<Vec<_>>();
    let horizontal_total = horizontal_lengths.iter().sum::<f64>();
    let direct = (shoulder.x - skeleton[0].x).hypot(shoulder.y - skeleton[0].y);
    if horizontal_total - direct > config.branch_max_distance_mm + 1.0e-7 {
        return None;
    }
    let tangent = config.overhang_angle_deg.to_radians().tan();
    if tangent <= 1.0e-9 {
        return None;
    }
    let required_drop = horizontal_total / tangent;
    let available = shoulder.z - trunk_floor;
    if required_drop > available + 1.0e-7 {
        return None;
    }
    let preferred_drop = required_drop
        .max(effective_contact_spacing(config) * 0.5)
        .min(available);
    let mut start_z = shoulder.z - preferred_drop;
    start_z = avoid_layer_section_plane_down(start_z, trunk_floor, layer_height_mm);
    let actual_drop = shoulder.z - start_z;
    if actual_drop + 1.0e-7 < required_drop {
        return None;
    }
    let mut cumulative = 0.0;
    let mut path = Vec::with_capacity(skeleton.len());
    for (index, point) in skeleton.into_iter().enumerate() {
        if index > 0 {
            cumulative += horizontal_lengths[index - 1];
        }
        let fraction = if horizontal_total <= 1.0e-9 {
            index as f64 / horizontal_lengths.len().max(1) as f64
        } else {
            cumulative / horizontal_total
        };
        path.push(Vec3 {
            x: point.x,
            y: point.y,
            z: start_z + actual_drop * fraction,
        });
    }
    if let Some(last) = path.last_mut() {
        *last = shoulder;
    }
    path_respects_branch_angle(&path, config.overhang_angle_deg).then_some(path)
}

fn avoid_layer_section_plane_down(z: f64, lower_limit: f64, layer_height_mm: f64) -> f64 {
    let layer = (z / layer_height_mm - 0.5).round().max(0.0);
    let section_z = (layer + 0.5) * layer_height_mm;
    if (z - section_z).abs() <= 1.0e-7 {
        (z - layer_height_mm * 0.2).max(lower_limit + layer_height_mm * 0.05)
    } else {
        z
    }
}

fn path_respects_branch_angle(path: &[Vec3], maximum_angle_deg: f64) -> bool {
    let tangent = maximum_angle_deg.to_radians().tan();
    path.windows(2).all(|segment| {
        let dz = segment[1].z - segment[0].z;
        let horizontal = (segment[1].x - segment[0].x).hypot(segment[1].y - segment[0].y);
        dz > 1.0e-7 && horizontal <= dz * tangent + 1.0e-6
    })
}

#[cfg(test)]
fn avoid_layer_section_plane(z: f64, upper_limit: f64, layer_height_mm: f64) -> f64 {
    let layer = (z / layer_height_mm - 0.5).round().max(0.0);
    let section_z = (layer + 0.5) * layer_height_mm;
    if (z - section_z).abs() <= 1.0e-7 {
        (z + layer_height_mm * 0.2).min(upper_limit - layer_height_mm * 0.05)
    } else {
        z
    }
}

fn compact_path(points: Vec<Vec3>) -> Vec<Vec3> {
    let mut compact = Vec::<Vec3>::with_capacity(points.len());
    for point in points {
        if compact
            .last()
            .is_none_or(|previous| distance(*previous, point) > 1.0e-6)
        {
            compact.push(point);
        }
    }
    compact
}

fn branch_path_is_clear(
    path: &[Vec3],
    radius: f64,
    build_volume_mm: [f64; 3],
    collision: &mut CollisionIndex<'_>,
    routing_grid: &RoutingGrid,
    maximum_angle_deg: f64,
) -> bool {
    let half_x = build_volume_mm[0] * 0.5;
    let half_y = build_volume_mm[1] * 0.5;
    path.iter().all(|point| {
        point.x - radius >= -half_x
            && point.x + radius <= half_x
            && point.y - radius >= -half_y
            && point.y + radius <= half_y
            && point.z >= 0.0
            && point.z <= build_volume_mm[2]
    }) && path_respects_branch_angle(path, maximum_angle_deg)
        && routing_grid.path_maybe_clear(path)
        && path
            .windows(2)
            .all(|segment| collision.capsule_clear(segment[0], segment[1], radius))
}

fn build_tree_root_pool(
    collision: &mut CollisionIndex<'_>,
    model: &Mesh,
    config: &SupportConfig,
    build_volume_mm: [f64; 3],
) -> Vec<Vec3> {
    let clearance = (config.tip_diameter_mm * 0.5).max(0.3);
    let base_radius = config.base_diameter_mm * 0.5;
    let trunk_radius = config.branch_max_trunk_diameter_mm * 0.5 + clearance;
    let spacing = config.base_diameter_mm + clearance;
    let half_x = build_volume_mm[0] * 0.5;
    let half_y = build_volume_mm[1] * 0.5;
    let usable_x = (build_volume_mm[0] - base_radius * 2.0).max(0.0);
    let usable_y = (build_volume_mm[1] - base_radius * 2.0).max(0.0);
    let columns = (usable_x / spacing).floor() as usize + 1;
    let rows = (usable_y / spacing).floor() as usize + 1;
    let first_x = -(columns.saturating_sub(1) as f64) * spacing * 0.5;
    let first_y = -(rows.saturating_sub(1) as f64) * spacing * 0.5;
    let model_top = model
        .triangles
        .iter()
        .flat_map(|triangle| triangle.0)
        .map(|point| point.z)
        .fold(0.0, f64::max)
        + clearance;
    let (model_minimum, model_maximum) = model.bounds();
    let mut roots = Vec::new();
    for column in 0..columns {
        for row in 0..rows {
            let root = Vec3 {
                x: first_x + column as f64 * spacing,
                y: first_y + row as f64 * spacing,
                z: 0.0,
            };
            if root.x - base_radius < -half_x
                || root.x + base_radius > half_x
                || root.y - base_radius < -half_y
                || root.y + base_radius > half_y
            {
                continue;
            }
            let outside_x = if root.x < model_minimum.x {
                model_minimum.x - root.x
            } else if root.x > model_maximum.x {
                root.x - model_maximum.x
            } else {
                0.0
            };
            let outside_y = if root.y < model_minimum.y {
                model_minimum.y - root.y
            } else if root.y > model_maximum.y {
                root.y - model_maximum.y
            } else {
                0.0
            };
            if outside_x.hypot(outside_y) > config.branch_max_distance_mm {
                continue;
            }
            let base_top = Vec3 {
                z: config.base_height_mm,
                ..root
            };
            let column_top = Vec3 {
                z: model_top,
                ..root
            };
            if collision.capsule_clear(root, base_top, base_radius + clearance)
                && collision.capsule_clear(base_top, column_top, trunk_radius)
            {
                roots.push(root);
            }
        }
    }
    roots
}

fn emit_tree_route(
    output: &mut Vec<Triangle>,
    route: &TreeRoute,
    supports: &[SupportAnchor],
    config: &SupportConfig,
) {
    let trunk_diameter = (config.trunk_diameter_mm * (route.contact_indices.len() as f64).sqrt())
        .clamp(
            config.trunk_diameter_mm,
            config.branch_max_trunk_diameter_mm,
        );
    let trunk_radius = trunk_diameter * 0.5;
    let base_radius = config.base_diameter_mm * 0.5;
    let trunk_top = route
        .paths
        .iter()
        .filter_map(|path| path.first())
        .map(|point| point.z)
        .fold(config.base_height_mm, f64::max);
    let base_top = config.base_height_mm.min(trunk_top);
    let transition_top = (base_top + trunk_diameter).min(trunk_top);
    let mut tiers = vec![(0.0, base_radius)];
    if base_top > 1.0e-9 {
        tiers.push((base_top, base_radius));
    }
    if transition_top > base_top + 1.0e-9 {
        tiers.push((transition_top, trunk_radius));
    }
    if trunk_top > tiers.last().expect("tree trunk has a tier").0 + 1.0e-9 {
        tiers.push((trunk_top, trunk_radius));
    }
    append_tiered_solid(
        output,
        route.root.x,
        route.root.y,
        &tiers,
        config.radial_segments.max(6) as usize,
        0.0,
    );

    let branch_radius = config.trunk_diameter_mm * 0.5;
    let shoulder_radius = branch_radius.max(config.tip_diameter_mm * 0.5);
    for (path, contact_index) in route.paths.iter().zip(&route.contact_indices) {
        let contact = supports[*contact_index].contact;
        for (segment_index, segment) in path.windows(2).enumerate() {
            let first_radius = if segment_index == 0 {
                branch_radius.min(trunk_radius)
            } else {
                shoulder_radius
            };
            let second_radius = if segment_index + 1 == path.len() - 1 {
                shoulder_radius
            } else {
                branch_radius.max(shoulder_radius)
            };
            append_strut(
                output,
                segment[0],
                segment[1],
                first_radius,
                second_radius,
                config.radial_segments.max(6) as usize,
            );
        }
        let shoulder = *path.last().expect("tree route has a shoulder");
        for (joint_index, joint) in path.iter().enumerate() {
            append_sphere(
                output,
                *joint,
                if joint_index == 0 {
                    trunk_radius
                } else {
                    shoulder_radius
                },
                config.radial_segments.max(6) as usize,
            );
        }
        append_strut(
            output,
            shoulder,
            Vec3 {
                x: contact.x,
                y: contact.y,
                z: contact.z + config.penetration_mm,
            },
            shoulder_radius,
            config.tip_diameter_mm * 0.5,
            config.radial_segments.max(6) as usize,
        );
    }
}

fn auto_contacts(model: &Mesh, config: &SupportConfig) -> Vec<SupportPoint> {
    let threshold = -config.overhang_angle_deg.to_radians().cos();
    let mut grid = HashMap::<ContactGridKey, Vec3>::new();
    let contact_spacing = effective_contact_spacing(config);
    // A single XY cell may contain several independent overhangs at different
    // heights (for example a lower plate plus a character above it). Keeping
    // only the lowest sample suppresses nearly all upper islands, so Z is part
    // of the sampling key for tree supports.
    let z_step = (contact_spacing * 0.5).max(config.tip_height_mm).max(1.0);
    for triangle in &model.triangles {
        let normal = triangle_normal(*triangle);
        if normal.z >= threshold {
            continue;
        }
        let area = triangle_area(*triangle);
        if area < 1.0e-8 {
            continue;
        }
        let longest = triangle
            .0
            .iter()
            .enumerate()
            .flat_map(|(first, point)| {
                triangle.0[first + 1..]
                    .iter()
                    .map(move |other| distance(*point, *other))
            })
            .fold(0.0, f64::max);
        let divisions = (longest / contact_spacing).ceil().clamp(1.0, 24.0) as usize;
        for first in 0..=divisions {
            for second in 0..=divisions - first {
                // Offset samples into the triangle so adjacent faces do not
                // create several contacts at the same vertex.
                let denominator = divisions as f64 + 3.0;
                let a = (first as f64 + 1.0) / denominator;
                let b = (second as f64 + 1.0) / denominator;
                let c = 1.0 - a - b;
                if c <= 0.0 {
                    continue;
                }
                let point = Vec3 {
                    x: triangle.0[0].x * a + triangle.0[1].x * b + triangle.0[2].x * c,
                    y: triangle.0[0].y * a + triangle.0[1].y * b + triangle.0[2].y * c,
                    z: triangle.0[0].z * a + triangle.0[1].z * b + triangle.0[2].z * c,
                };
                if point.z <= config.tip_height_mm {
                    continue;
                }
                let key = ContactGridKey(
                    (point.x / contact_spacing).round() as i64,
                    (point.y / contact_spacing).round() as i64,
                    (point.z / z_step).round() as i64,
                );
                grid.entry(key)
                    .and_modify(|existing| {
                        if point.z < existing.z {
                            *existing = point;
                        }
                    })
                    .or_insert(point);
            }
        }
    }
    grid.into_values()
        .map(|point| SupportPoint {
            contact_mm: [point.x, point.y, point.z],
            base_z_mm: None,
        })
        .collect()
}

fn effective_contact_spacing(config: &SupportConfig) -> f64 {
    // 1 cm² = 100 mm². Density is an upper bound, while spacing remains an
    // intuitive minimum-distance control; the sparser of the two wins.
    let density_spacing = (100.0 / config.density_per_cm2.max(1.0e-9)).sqrt();
    config.spacing_mm.max(density_spacing)
}

fn highest_surface_below(
    index: &super::spatial::TriangleIndex<'_>,
    minimum_z: f64,
    x: f64,
    y: f64,
    contact_z: f64,
) -> Option<f64> {
    let top = contact_z - 0.5;
    if top < minimum_z {
        return None;
    }
    let mut highest: Option<f64> = None;
    index.any_hit(
        Vec3 { x, y, z: minimum_z },
        Vec3 { x, y, z: top },
        0.0,
        |triangle| {
            if let Some(z) = vertical_intersection(triangle, x, y).filter(|z| *z < top) {
                highest = Some(highest.map_or(z, |old| old.max(z)));
            }
            false // Visit every candidate; do not stop at the first surface.
        },
    );
    highest
}

fn vertical_intersection(triangle: Triangle, x: f64, y: f64) -> Option<f64> {
    let [a, b, c] = triangle.0;
    let denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if denominator.abs() < 1.0e-12 {
        return None;
    }
    let first = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / denominator;
    let second = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / denominator;
    let third = 1.0 - first - second;
    if first >= -1.0e-8 && second >= -1.0e-8 && third >= -1.0e-8 {
        Some(first * a.z + second * b.z + third * c.z)
    } else {
        None
    }
}

fn append_pillar(output: &mut Vec<Triangle>, contact: Vec3, base_z: f64, config: &SupportConfig) {
    let top_z = contact.z + config.penetration_mm;
    let base_on_platform = base_z <= 1.0e-6;
    let lower_trunk_z = if base_on_platform {
        (base_z + config.base_height_mm).min(top_z)
    } else {
        base_z
    };
    let upper_trunk_z = (top_z - config.tip_height_mm).max(lower_trunk_z);
    let trunk_radius = config.trunk_diameter_mm * 0.5;
    let tip_radius = config.tip_diameter_mm * 0.5;
    let base_radius = config.base_diameter_mm * 0.5;
    let lower_radius = match config.shape {
        SupportShape::Cylinder | SupportShape::Square => trunk_radius,
        SupportShape::Branch | SupportShape::Tapered => (trunk_radius * 1.35).min(base_radius),
    };
    let sides = match config.shape {
        SupportShape::Square => 4,
        _ => config.radial_segments.max(6) as usize,
    };
    let rotation = if config.shape == SupportShape::Square {
        std::f64::consts::FRAC_PI_4
    } else {
        0.0
    };

    let mut tiers = Vec::<(f64, f64)>::new();
    if base_on_platform {
        tiers.push((base_z, base_radius));
        tiers.push((lower_trunk_z, base_radius));
    } else {
        tiers.push((base_z, lower_radius));
    }
    if tiers
        .last()
        .is_none_or(|(z, radius)| (*z - lower_trunk_z).abs() > 1.0e-9 || *radius != lower_radius)
    {
        tiers.push((lower_trunk_z, lower_radius));
    }
    if upper_trunk_z > lower_trunk_z + 1.0e-9 {
        tiers.push((upper_trunk_z, trunk_radius));
    }
    tiers.push((top_z, tip_radius));
    append_tiered_solid(output, contact.x, contact.y, &tiers, sides, rotation);
}

fn append_tiered_solid(
    output: &mut Vec<Triangle>,
    x: f64,
    y: f64,
    tiers: &[(f64, f64)],
    sides: usize,
    rotation: f64,
) {
    let rings = tiers
        .iter()
        .map(|(z, radius)| {
            (0..sides)
                .map(|index| {
                    let angle = rotation + index as f64 * std::f64::consts::TAU / sides as f64;
                    Vec3 {
                        x: x + radius * angle.cos(),
                        y: y + radius * angle.sin(),
                        z: *z,
                    }
                })
                .collect::<Vec<_>>()
        })
        .collect::<Vec<_>>();
    for pair in rings.windows(2) {
        for index in 0..sides {
            let next = (index + 1) % sides;
            output.push(Triangle([pair[0][index], pair[0][next], pair[1][next]]));
            output.push(Triangle([pair[0][index], pair[1][next], pair[1][index]]));
        }
    }
    let bottom = Vec3 {
        x,
        y,
        z: tiers[0].0,
    };
    let top = Vec3 {
        x,
        y,
        z: tiers.last().expect("pillar has tiers").0,
    };
    for index in 0..sides {
        let next = (index + 1) % sides;
        output.push(Triangle([bottom, rings[0][next], rings[0][index]]));
        let last = rings.last().expect("pillar has rings");
        output.push(Triangle([top, last[index], last[next]]));
    }
}

fn append_strut(
    output: &mut Vec<Triangle>,
    start: Vec3,
    end: Vec3,
    start_radius: f64,
    end_radius: f64,
    sides: usize,
) {
    let axis = Vec3 {
        x: end.x - start.x,
        y: end.y - start.y,
        z: end.z - start.z,
    };
    let length = (axis.x * axis.x + axis.y * axis.y + axis.z * axis.z).sqrt();
    if length <= 1.0e-9 {
        return;
    }
    let direction = Vec3 {
        x: axis.x / length,
        y: axis.y / length,
        z: axis.z / length,
    };
    let reference = if direction.z.abs() < 0.9 {
        Vec3 {
            x: 0.0,
            y: 0.0,
            z: 1.0,
        }
    } else {
        Vec3 {
            x: 1.0,
            y: 0.0,
            z: 0.0,
        }
    };
    let first = normalize(cross(direction, reference));
    let second = normalize(cross(direction, first));
    let ring = |center: Vec3, radius: f64| {
        (0..sides)
            .map(|index| {
                let angle = (index as f64 + 0.5) * std::f64::consts::TAU / sides as f64;
                Vec3 {
                    x: center.x + radius * (first.x * angle.cos() + second.x * angle.sin()),
                    y: center.y + radius * (first.y * angle.cos() + second.y * angle.sin()),
                    z: center.z + radius * (first.z * angle.cos() + second.z * angle.sin()),
                }
            })
            .collect::<Vec<_>>()
    };
    let bottom = ring(start, start_radius);
    let top = ring(end, end_radius);
    for index in 0..sides {
        let next = (index + 1) % sides;
        output.push(Triangle([bottom[index], bottom[next], top[next]]));
        output.push(Triangle([bottom[index], top[next], top[index]]));
        output.push(Triangle([start, bottom[next], bottom[index]]));
        output.push(Triangle([end, top[index], top[next]]));
    }
}

fn append_sphere(output: &mut Vec<Triangle>, center: Vec3, radius: f64, sides: usize) {
    let latitudes = (sides / 2).max(4);
    let rings = (1..latitudes)
        .map(|latitude| {
            let polar = latitude as f64 * std::f64::consts::PI / latitudes as f64;
            let ring_radius = radius * polar.sin();
            let z = center.z + radius * polar.cos();
            (0..sides)
                .map(|longitude| {
                    let azimuth = longitude as f64 * std::f64::consts::TAU / sides as f64;
                    Vec3 {
                        x: center.x + ring_radius * azimuth.cos(),
                        y: center.y + ring_radius * azimuth.sin(),
                        z,
                    }
                })
                .collect::<Vec<_>>()
        })
        .collect::<Vec<_>>();
    let north = Vec3 {
        z: center.z + radius,
        ..center
    };
    let south = Vec3 {
        z: center.z - radius,
        ..center
    };
    let first = &rings[0];
    let last = rings.last().expect("sphere has latitude rings");
    for longitude in 0..sides {
        let next = (longitude + 1) % sides;
        output.push(Triangle([north, first[longitude], first[next]]));
        output.push(Triangle([south, last[next], last[longitude]]));
    }
    for pair in rings.windows(2) {
        for longitude in 0..sides {
            let next = (longitude + 1) % sides;
            output.push(Triangle([
                pair[0][longitude],
                pair[1][longitude],
                pair[1][next],
            ]));
            output.push(Triangle([pair[0][longitude], pair[1][next], pair[0][next]]));
        }
    }
}

fn segment_triangle_distance_squared(start: Vec3, end: Vec3, triangle: Triangle) -> f64 {
    if segment_intersects_triangle(start, end, triangle) {
        return 0.0;
    }
    let [a, b, c] = triangle.0;
    point_triangle_distance_squared(start, triangle)
        .min(point_triangle_distance_squared(end, triangle))
        .min(segment_segment_distance_squared(start, end, a, b))
        .min(segment_segment_distance_squared(start, end, b, c))
        .min(segment_segment_distance_squared(start, end, c, a))
}

fn segment_intersects_triangle(start: Vec3, end: Vec3, triangle: Triangle) -> bool {
    let [a, b, c] = triangle.0;
    let direction = subtract(end, start);
    let edge_ab = subtract(b, a);
    let edge_ac = subtract(c, a);
    let perpendicular = cross(direction, edge_ac);
    let determinant = dot(edge_ab, perpendicular);
    if determinant.abs() <= 1.0e-12 {
        return false;
    }
    let inverse = 1.0 / determinant;
    let from_a = subtract(start, a);
    let u = inverse * dot(from_a, perpendicular);
    if !(-1.0e-10..=1.0 + 1.0e-10).contains(&u) {
        return false;
    }
    let q = cross(from_a, edge_ab);
    let v = inverse * dot(direction, q);
    if v < -1.0e-10 || u + v > 1.0 + 1.0e-10 {
        return false;
    }
    let along_segment = inverse * dot(edge_ac, q);
    (-1.0e-10..=1.0 + 1.0e-10).contains(&along_segment)
}

fn point_triangle_distance_squared(point: Vec3, triangle: Triangle) -> f64 {
    let [a, b, c] = triangle.0;
    let ab = subtract(b, a);
    let ac = subtract(c, a);
    if dot(cross(ab, ac), cross(ab, ac)) <= 1.0e-24 {
        return point_segment_distance_squared(point, a, b)
            .min(point_segment_distance_squared(point, b, c))
            .min(point_segment_distance_squared(point, c, a));
    }
    let ap = subtract(point, a);
    let d1 = dot(ab, ap);
    let d2 = dot(ac, ap);
    if d1 <= 0.0 && d2 <= 0.0 {
        return dot(ap, ap);
    }

    let bp = subtract(point, b);
    let d3 = dot(ab, bp);
    let d4 = dot(ac, bp);
    if d3 >= 0.0 && d4 <= d3 {
        return dot(bp, bp);
    }

    let edge_ab_region = d1 * d4 - d3 * d2;
    if edge_ab_region <= 0.0 && d1 >= 0.0 && d3 <= 0.0 {
        let fraction = d1 / (d1 - d3);
        return dot(
            subtract(point, add_scaled(a, ab, fraction)),
            subtract(point, add_scaled(a, ab, fraction)),
        );
    }

    let cp = subtract(point, c);
    let d5 = dot(ab, cp);
    let d6 = dot(ac, cp);
    if d6 >= 0.0 && d5 <= d6 {
        return dot(cp, cp);
    }

    let edge_ac_region = d5 * d2 - d1 * d6;
    if edge_ac_region <= 0.0 && d2 >= 0.0 && d6 <= 0.0 {
        let fraction = d2 / (d2 - d6);
        let closest = add_scaled(a, ac, fraction);
        let delta = subtract(point, closest);
        return dot(delta, delta);
    }

    let edge_bc_region = d3 * d6 - d5 * d4;
    if edge_bc_region <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0 {
        let fraction = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        let closest = add_scaled(b, subtract(c, b), fraction);
        let delta = subtract(point, closest);
        return dot(delta, delta);
    }

    let denominator = 1.0 / (edge_ab_region + edge_ac_region + edge_bc_region);
    let v = edge_ac_region * denominator;
    let w = edge_ab_region * denominator;
    let closest = Vec3 {
        x: a.x + ab.x * v + ac.x * w,
        y: a.y + ab.y * v + ac.y * w,
        z: a.z + ab.z * v + ac.z * w,
    };
    let delta = subtract(point, closest);
    dot(delta, delta)
}

fn segment_segment_distance_squared(
    first_start: Vec3,
    first_end: Vec3,
    second_start: Vec3,
    second_end: Vec3,
) -> f64 {
    let first = subtract(first_end, first_start);
    let second = subtract(second_end, second_start);
    let between = subtract(first_start, second_start);
    let first_length = dot(first, first);
    let second_length = dot(second, second);
    let epsilon = 1.0e-18;
    let (mut first_fraction, mut second_fraction);
    if first_length <= epsilon && second_length <= epsilon {
        return dot(between, between);
    }
    if first_length <= epsilon {
        first_fraction = 0.0;
        second_fraction = (dot(second, between) / second_length).clamp(0.0, 1.0);
    } else {
        let first_projection = dot(first, between);
        if second_length <= epsilon {
            second_fraction = 0.0;
            first_fraction = (-first_projection / first_length).clamp(0.0, 1.0);
        } else {
            let shared_projection = dot(first, second);
            let second_projection = dot(second, between);
            let denominator = first_length * second_length - shared_projection * shared_projection;
            first_fraction = if denominator.abs() > epsilon {
                ((shared_projection * second_projection - first_projection * second_length)
                    / denominator)
                    .clamp(0.0, 1.0)
            } else {
                0.0
            };
            second_fraction =
                (shared_projection * first_fraction + second_projection) / second_length;
            if second_fraction < 0.0 {
                second_fraction = 0.0;
                first_fraction = (-first_projection / first_length).clamp(0.0, 1.0);
            } else if second_fraction > 1.0 {
                second_fraction = 1.0;
                first_fraction =
                    ((shared_projection - first_projection) / first_length).clamp(0.0, 1.0);
            }
        }
    }
    let first_point = add_scaled(first_start, first, first_fraction);
    let second_point = add_scaled(second_start, second, second_fraction);
    let delta = subtract(first_point, second_point);
    dot(delta, delta)
}

fn point_segment_distance_squared(point: Vec3, start: Vec3, end: Vec3) -> f64 {
    let segment = subtract(end, start);
    let length_squared = dot(segment, segment);
    let fraction = if length_squared <= 1.0e-18 {
        0.0
    } else {
        (dot(subtract(point, start), segment) / length_squared).clamp(0.0, 1.0)
    };
    let delta = subtract(point, add_scaled(start, segment, fraction));
    dot(delta, delta)
}

fn subtract(first: Vec3, second: Vec3) -> Vec3 {
    Vec3 {
        x: first.x - second.x,
        y: first.y - second.y,
        z: first.z - second.z,
    }
}

fn add_scaled(origin: Vec3, direction: Vec3, scale: f64) -> Vec3 {
    Vec3 {
        x: origin.x + direction.x * scale,
        y: origin.y + direction.y * scale,
        z: origin.z + direction.z * scale,
    }
}

fn dot(first: Vec3, second: Vec3) -> f64 {
    first.x * second.x + first.y * second.y + first.z * second.z
}

fn cross(first: Vec3, second: Vec3) -> Vec3 {
    Vec3 {
        x: first.y * second.z - first.z * second.y,
        y: first.z * second.x - first.x * second.z,
        z: first.x * second.y - first.y * second.x,
    }
}

fn triangle_normal(triangle: Triangle) -> Vec3 {
    let [a, b, c] = triangle.0;
    let first = Vec3 {
        x: b.x - a.x,
        y: b.y - a.y,
        z: b.z - a.z,
    };
    let second = Vec3 {
        x: c.x - a.x,
        y: c.y - a.y,
        z: c.z - a.z,
    };
    normalize(Vec3 {
        x: first.y * second.z - first.z * second.y,
        y: first.z * second.x - first.x * second.z,
        z: first.x * second.y - first.y * second.x,
    })
}

fn triangle_area(triangle: Triangle) -> f64 {
    let normal = triangle_normal_unnormalized(triangle);
    (normal.x * normal.x + normal.y * normal.y + normal.z * normal.z).sqrt() * 0.5
}

fn triangle_normal_unnormalized(triangle: Triangle) -> Vec3 {
    let [a, b, c] = triangle.0;
    Vec3 {
        x: (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y),
        y: (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z),
        z: (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x),
    }
}

fn normalize(point: Vec3) -> Vec3 {
    let length = (point.x * point.x + point.y * point.y + point.z * point.z).sqrt();
    if length == 0.0 {
        Vec3::default()
    } else {
        Vec3 {
            x: point.x / length,
            y: point.y / length,
            z: point.z / length,
        }
    }
}

fn distance(first: Vec3, second: Vec3) -> f64 {
    ((first.x - second.x).powi(2) + (first.y - second.y).powi(2) + (first.z - second.z).powi(2))
        .sqrt()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bvh_capsule_checks_match_exhaustive_triangle_tests() {
        let mut state = 12345_u64;
        let mut coordinate = || {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            ((state >> 32) as u32 as f64 / u32::MAX as f64 - 0.5) * 40.0
        };
        let mut point = || Vec3 {
            x: coordinate(),
            y: coordinate(),
            z: coordinate(),
        };
        let mesh = Mesh {
            triangles: (0..192)
                .map(|_| Triangle([point(), point(), point()]))
                .collect(),
        };
        let index = CollisionIndex::new(&mesh);
        for query in 0..512 {
            let start = point();
            let mut end = point();
            match query % 8 {
                0 => end = start,
                1 => end.x = start.x,
                2 => {
                    end.x = start.x;
                    end.y = start.y;
                }
                _ => {}
            }
            let radius = [0.0, 0.05, 0.5, 2.0][query % 4];
            let brute_clear = mesh
                .triangles
                .iter()
                .all(|t| segment_triangle_distance_squared(start, end, *t) > radius * radius);
            assert_eq!(
                index.capsule_clear(start, end, radius),
                brute_clear,
                "query {query}"
            );
        }
    }

    #[test]
    fn indexed_surface_lookup_preserves_edge_tolerance_and_highest_hit() {
        let model = Mesh {
            triangles: vec![
                horizontal_triangle(5.0, false),
                horizontal_triangle(12.0, false),
            ],
        };
        let index = super::super::spatial::TriangleIndex::new(&model);
        for (x, y) in [
            (0.0, 0.0),
            (10.0, -10.0),
            (10.0 + 1.0e-8, -10.0),
            (30.0, 0.0),
        ] {
            for top in [5.0, 5.5, 8.0, 12.5, 15.0] {
                let brute = model
                    .triangles
                    .iter()
                    .filter_map(|t| vertical_intersection(*t, x, y))
                    .filter(|z| *z < top - 0.5)
                    .max_by(f64::total_cmp);
                assert_eq!(highest_surface_below(&index, 5.0, x, y, top), brute);
            }
        }
    }

    fn horizontal_triangle(z: f64, downward: bool) -> Triangle {
        let points = [
            Vec3 {
                x: -10.0,
                y: -10.0,
                z,
            },
            Vec3 { x: 0.0, y: 10.0, z },
            Vec3 {
                x: 10.0,
                y: -10.0,
                z,
            },
        ];
        if downward {
            Triangle(points)
        } else {
            Triangle([points[0], points[2], points[1]])
        }
    }

    #[test]
    fn creates_closed_support_pillar() {
        let config = SupportConfig {
            enabled: true,
            ..SupportConfig::medium()
        };
        let mut triangles = Vec::new();
        append_pillar(
            &mut triangles,
            Vec3 {
                x: 0.0,
                y: 0.0,
                z: 10.0,
            },
            0.0,
            &config,
        );
        assert!(!triangles.is_empty());
        assert!(
            triangles
                .iter()
                .flat_map(|triangle| triangle.0.iter())
                .all(|point| point.z >= 0.0)
        );
    }

    #[test]
    fn branch_supports_share_a_trunk_and_create_inclined_arms() {
        let config = SupportConfig {
            enabled: true,
            shape: SupportShape::Branch,
            ..SupportConfig::medium()
        };
        let supports = [
            SupportAnchor {
                contact: Vec3 {
                    x: -2.0,
                    y: 0.0,
                    z: 15.0,
                },
                base_z: 0.0,
                automatic: true,
            },
            SupportAnchor {
                contact: Vec3 {
                    x: 2.0,
                    y: 0.0,
                    z: 15.0,
                },
                base_z: 0.0,
                automatic: true,
            },
        ];
        let model = Mesh {
            triangles: vec![horizontal_triangle(15.0, true)],
        };
        let mut triangles = Vec::new();
        let count = append_branch_forest(
            &mut triangles,
            &model,
            &supports,
            &config,
            [100.0, 100.0, 100.0],
            0.05,
        );
        assert_eq!(count, 2);
        assert!(!triangles.is_empty());
        assert!(triangles.iter().any(|triangle| {
            let [first, second, _] = triangle.0;
            (first.x - second.x).abs() > 1.0e-4 && (first.z - second.z).abs() > 1.0e-4
        }));
    }

    #[test]
    fn graded_branch_segments_never_exceed_the_configured_angle() {
        let config = SupportConfig {
            overhang_angle_deg: 35.0,
            ..SupportConfig::medium()
        };
        let shoulder = Vec3 {
            x: 12.0,
            y: 0.0,
            z: 30.0,
        };
        let path = grade_branch_skeleton(
            vec![
                Vec3 {
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                },
                Vec3 {
                    x: 4.0,
                    y: 3.0,
                    z: 0.0,
                },
                Vec3 {
                    x: 8.0,
                    y: 3.0,
                    z: 0.0,
                },
                shoulder,
            ],
            shoulder,
            2.0,
            &config,
            0.05,
        )
        .expect("the available height should allow an angle-limited branch");

        assert!(path_respects_branch_angle(&path, config.overhang_angle_deg));
        for segment in path.windows(2) {
            let horizontal = (segment[1].x - segment[0].x).hypot(segment[1].y - segment[0].y);
            let vertical = segment[1].z - segment[0].z;
            let angle = horizontal.atan2(vertical).to_degrees();
            assert!(angle <= config.overhang_angle_deg + 1.0e-6);
        }
    }

    #[test]
    fn branch_detour_limit_rejects_a_full_tour_around_the_model() {
        let config = SupportConfig {
            overhang_angle_deg: 60.0,
            branch_max_distance_mm: 2.0,
            ..SupportConfig::medium()
        };
        let shoulder = Vec3 {
            x: 5.0,
            y: 0.0,
            z: 40.0,
        };
        let path = grade_branch_skeleton(
            vec![
                Vec3 {
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                },
                Vec3 {
                    x: 0.0,
                    y: 20.0,
                    z: 0.0,
                },
                shoulder,
            ],
            shoulder,
            2.0,
            &config,
            0.05,
        );
        assert!(path.is_none());
    }

    #[test]
    fn support_density_is_an_upper_bound_on_contact_spacing() {
        let sparse = SupportConfig {
            spacing_mm: 2.0,
            density_per_cm2: 1.0,
            ..SupportConfig::medium()
        };
        assert!((effective_contact_spacing(&sparse) - 10.0).abs() < 1.0e-9);

        let spacing_limited = SupportConfig {
            spacing_mm: 4.0,
            density_per_cm2: 100.0,
            ..SupportConfig::medium()
        };
        assert!((effective_contact_spacing(&spacing_limited) - 4.0).abs() < 1.0e-9);
    }

    #[test]
    fn nearby_contacts_are_filtered_before_solids_are_built() {
        let point = |x| CandidateContact {
            point: SupportPoint {
                contact_mm: [x, 0.0, 10.0],
                base_z_mm: None,
            },
            automatic: true,
        };
        let filtered = separate_contacts(vec![point(0.0), point(0.1), point(3.0)], 1.0, None);
        assert_eq!(filtered.len(), 2);
    }

    #[test]
    fn automatic_contacts_leave_routing_to_the_tree_builder() {
        let model = Mesh {
            triangles: vec![horizontal_triangle(10.0, true)],
        };
        let contacts = auto_contacts(&model, &SupportConfig::medium());
        assert!(!contacts.is_empty());
        assert!(contacts.iter().all(|contact| contact.base_z_mm.is_none()));
    }

    #[test]
    fn automatic_contacts_preserve_separate_overhang_heights() {
        let model = Mesh {
            triangles: vec![
                horizontal_triangle(10.0, true),
                horizontal_triangle(20.0, true),
            ],
        };
        let contacts = auto_contacts(&model, &SupportConfig::medium());
        assert!(
            contacts
                .iter()
                .any(|contact| (contact.contact_mm[2] - 10.0).abs() < 1.0e-6)
        );
        assert!(
            contacts
                .iter()
                .any(|contact| (contact.contact_mm[2] - 20.0).abs() < 1.0e-6)
        );
    }

    #[test]
    fn blocked_automatic_contacts_route_to_a_root_outside_the_model() {
        let model = Mesh {
            triangles: vec![
                horizontal_triangle(5.0, false),
                horizontal_triangle(15.0, true),
            ],
        };
        let config = SupportConfig {
            enabled: true,
            shape: SupportShape::Branch,
            ..SupportConfig::medium()
        };
        let generated = generate_supports(&model, &config, [100.0, 100.0, 100.0], 0.05)
            .expect("support generation should succeed");
        assert!(generated.count > 0);
        assert!(generated.mesh.triangles.iter().any(|triangle| {
            triangle.0.iter().any(|point| {
                point.z.abs() < 1.0e-8 && (point.x.abs() > 10.0 || point.y.abs() > 10.0)
            })
        }));
    }

    #[test]
    fn pillar_spacing_accounts_for_the_full_base_diameter() {
        let config = SupportConfig::medium();
        let point = |x| CandidateContact {
            point: SupportPoint {
                contact_mm: [x, 0.0, 10.0],
                base_z_mm: None,
            },
            automatic: true,
        };
        let clearance = (config.tip_diameter_mm * 0.5).max(0.3);
        let filtered = separate_contacts(
            vec![point(0.0), point(4.0), point(5.0)],
            config.base_diameter_mm + clearance,
            None,
        );
        assert_eq!(filtered.len(), 2);
    }

    #[test]
    fn planar_router_walks_around_an_occupied_wall() {
        let lower_left = Vec3 {
            x: 0.0,
            y: -3.0,
            z: 0.0,
        };
        let lower_right = Vec3 {
            x: 0.0,
            y: 3.0,
            z: 0.0,
        };
        let upper_left = Vec3 {
            x: 0.0,
            y: -3.0,
            z: 10.0,
        };
        let upper_right = Vec3 {
            x: 0.0,
            y: 3.0,
            z: 10.0,
        };
        let wall = Mesh {
            triangles: vec![
                Triangle([lower_left, lower_right, upper_right]),
                Triangle([lower_left, upper_right, upper_left]),
            ],
        };
        let grid = RoutingGrid::new(&wall, [20.0, 20.0, 20.0], 0.5, 4.0);
        let path = grid
            .planar_path(
                Vec3 {
                    x: -5.0,
                    y: 0.0,
                    z: 5.0,
                },
                Vec3 {
                    x: 5.0,
                    y: 0.0,
                    z: 5.0,
                },
                1_000,
            )
            .expect("a route should exist around the finite wall");
        assert!(path.iter().any(|point| point.y.abs() > 3.0));
    }

    #[test]
    fn branch_nodes_do_not_coincide_with_layer_sections() {
        let shifted = avoid_layer_section_plane(35.075, 40.0, 0.05);
        assert!((shifted - 35.085).abs() < 1.0e-9);
        assert_eq!(avoid_layer_section_plane(35.07, 40.0, 0.05), 35.07);
    }

    #[test]
    fn capsule_distance_detects_a_triangle_crossing() {
        let triangle = horizontal_triangle(5.0, false);
        assert_eq!(
            segment_triangle_distance_squared(
                Vec3 {
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                },
                Vec3 {
                    x: 0.0,
                    y: 0.0,
                    z: 10.0,
                },
                triangle,
            ),
            0.0
        );
    }
}
