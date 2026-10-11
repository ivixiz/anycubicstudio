//! Conservative 3D broad phase. Exact triangle tests remain authoritative.

use super::mesh::{Mesh, Triangle, Vec3};

#[derive(Clone, Copy)]
struct Bounds {
    min: [f64; 3],
    max: [f64; 3],
}

impl Bounds {
    fn triangle(triangle: Triangle) -> Self {
        let points = triangle.0.map(|p| [p.x, p.y, p.z]);
        let mut bounds = Self {
            min: std::array::from_fn(|a| points[0][a].min(points[1][a]).min(points[2][a])),
            max: std::array::from_fn(|a| points[0][a].max(points[1][a]).max(points[2][a])),
        };
        // The vertical surface test accepts barycentric coordinates up to
        // 1e-8 outside an edge. Its broad phase must include that same fringe.
        for axis in 0..3 {
            let padding = (bounds.max[axis] - bounds.min[axis]) * 2.0e-8;
            bounds.min[axis] -= padding;
            bounds.max[axis] += padding;
        }
        bounds
    }

    fn union(self, other: Self) -> Self {
        Self {
            min: std::array::from_fn(|a| self.min[a].min(other.min[a])),
            max: std::array::from_fn(|a| self.max[a].max(other.max[a])),
        }
    }
}

struct Node {
    bounds: Bounds,
    // Branch: left/right child. Leaf: first triangle offset/count.
    first: usize,
    second: usize,
    leaf: bool,
}

pub(super) struct TriangleIndex<'a> {
    triangles: &'a [Triangle],
    indices: Vec<usize>,
    nodes: Vec<Node>,
}

impl<'a> TriangleIndex<'a> {
    pub(super) fn new(mesh: &'a Mesh) -> Self {
        let bounds = mesh
            .triangles
            .iter()
            .copied()
            .map(Bounds::triangle)
            .collect::<Vec<_>>();
        let mut index = Self {
            triangles: &mesh.triangles,
            indices: (0..mesh.triangles.len()).collect(),
            nodes: Vec::with_capacity(mesh.triangles.len() / 4),
        };
        if !bounds.is_empty() {
            Self::build(&mut index.nodes, &mut index.indices, 0, &bounds);
        }
        index
    }

    fn build(
        nodes: &mut Vec<Node>,
        indices: &mut [usize],
        offset: usize,
        bounds: &[Bounds],
    ) -> usize {
        let combined = indices
            .iter()
            .skip(1)
            .fold(bounds[indices[0]], |b, i| b.union(bounds[*i]));
        let id = nodes.len();
        nodes.push(Node {
            bounds: combined,
            first: offset,
            second: indices.len(),
            leaf: true,
        });
        if indices.len() > 12 {
            let axis = (0..3)
                .max_by(|a, b| {
                    (combined.max[*a] - combined.min[*a])
                        .total_cmp(&(combined.max[*b] - combined.min[*b]))
                })
                .unwrap();
            let middle = indices.len() / 2;
            indices.select_nth_unstable_by(middle, |a, b| {
                (bounds[*a].min[axis] + bounds[*a].max[axis])
                    .total_cmp(&(bounds[*b].min[axis] + bounds[*b].max[axis]))
                    .then_with(|| a.cmp(b))
            });
            let (left, right) = indices.split_at_mut(middle);
            let first = Self::build(nodes, left, offset, bounds);
            let second = Self::build(nodes, right, offset + middle, bounds);
            nodes[id].first = first;
            nodes[id].second = second;
            nodes[id].leaf = false;
        }
        id
    }

    /// Expanding each AABB by the capsule radius is conservative: no capsule
    /// hit can be lost, while remote faces in XY *and* Z are skipped together.
    pub(super) fn any_hit(
        &self,
        start: Vec3,
        end: Vec3,
        radius: f64,
        mut exact_hit: impl FnMut(Triangle) -> bool,
    ) -> bool {
        if self.nodes.is_empty() {
            return false;
        }
        let query = SegmentQuery::new(start, end, radius);
        self.visit(0, &query, &mut exact_hit)
    }

    fn visit(
        &self,
        id: usize,
        query: &SegmentQuery,
        exact_hit: &mut impl FnMut(Triangle) -> bool,
    ) -> bool {
        let node = &self.nodes[id];
        if !query.intersects(node.bounds) {
            return false;
        }
        if node.leaf {
            self.indices[node.first..node.first + node.second]
                .iter()
                .any(|i| exact_hit(self.triangles[*i]))
        } else {
            self.visit(node.first, query, exact_hit) || self.visit(node.second, query, exact_hit)
        }
    }
}

struct SegmentQuery {
    origin: [f64; 3],
    inverse: [f64; 3],
    parallel: [bool; 3],
    radius: f64,
}

impl SegmentQuery {
    fn new(start: Vec3, end: Vec3, radius: f64) -> Self {
        let delta = [end.x - start.x, end.y - start.y, end.z - start.z];
        Self {
            origin: [start.x, start.y, start.z],
            inverse: delta.map(|v| if v == 0.0 { 0.0 } else { 1.0 / v }),
            parallel: delta.map(|v| v == 0.0),
            radius: radius.max(0.0) + 1.0e-9,
        }
    }

    fn intersects(&self, bounds: Bounds) -> bool {
        let mut near: f64 = 0.0;
        let mut far: f64 = 1.0;
        for axis in 0..3 {
            let low = bounds.min[axis] - self.radius - self.origin[axis];
            let high = bounds.max[axis] + self.radius - self.origin[axis];
            if self.parallel[axis] {
                if low > 0.0 || high < 0.0 {
                    return false;
                }
            } else {
                let first = low * self.inverse[axis];
                let second = high * self.inverse[axis];
                near = near.max(first.min(second));
                far = far.min(first.max(second));
                if near > far {
                    return false;
                }
            }
        }
        true
    }
}
