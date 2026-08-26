use std::collections::HashMap;

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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct GridKey(i64, i64);

pub(crate) fn generate_supports(model: &Mesh, config: &SupportConfig) -> Result<GeneratedSupports> {
    if !config.enabled {
        return Ok(GeneratedSupports {
            mesh: Mesh {
                triangles: Vec::new(),
            },
            count: 0,
        });
    }

    let mut contacts = config.manual_points.clone();
    if config.auto_generate {
        contacts.extend(auto_contacts(model, config));
    }
    contacts.sort_by(|left, right| {
        left.contact_mm[0]
            .total_cmp(&right.contact_mm[0])
            .then_with(|| left.contact_mm[1].total_cmp(&right.contact_mm[1]))
            .then_with(|| left.contact_mm[2].total_cmp(&right.contact_mm[2]))
    });
    contacts.dedup_by(|left, right| {
        (left.contact_mm[0] - right.contact_mm[0]).abs() < config.spacing_mm * 0.25
            && (left.contact_mm[1] - right.contact_mm[1]).abs() < config.spacing_mm * 0.25
            && (left.contact_mm[2] - right.contact_mm[2]).abs() < config.tip_height_mm
    });

    let mut triangles = Vec::new();
    let mut retained = Vec::new();
    for mut support in contacts {
        let contact = Vec3 {
            x: support.contact_mm[0],
            y: support.contact_mm[1],
            z: support.contact_mm[2],
        };
        if !contact.x.is_finite() || !contact.y.is_finite() || !contact.z.is_finite() {
            return Err(Error::InvalidModel(
                "support contact contains a non-finite coordinate".into(),
            ));
        }
        let base_z = support.base_z_mm.unwrap_or_else(|| {
            highest_surface_below(model, contact.x, contact.y, contact.z).unwrap_or(0.0)
        });
        support.base_z_mm = Some(base_z);
        if base_z < -1.0e-6
            || contact.z - base_z < config.min_height_mm
            || contact.z - base_z > config.max_height_mm
        {
            continue;
        }
        append_pillar(&mut triangles, contact, base_z.max(0.0), config);
        retained.push(support);
    }

    Ok(GeneratedSupports {
        count: retained.len(),
        mesh: Mesh { triangles },
    })
}

fn auto_contacts(model: &Mesh, config: &SupportConfig) -> Vec<SupportPoint> {
    let threshold = -config.overhang_angle_deg.to_radians().cos();
    let mut grid = HashMap::<GridKey, Vec3>::new();
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
        let divisions = (longest / config.spacing_mm).ceil().clamp(1.0, 24.0) as usize;
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
                let key = GridKey(
                    (point.x / config.spacing_mm).round() as i64,
                    (point.y / config.spacing_mm).round() as i64,
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

fn highest_surface_below(model: &Mesh, x: f64, y: f64, contact_z: f64) -> Option<f64> {
    model
        .triangles
        .iter()
        .filter_map(|triangle| vertical_intersection(*triangle, x, y))
        .filter(|z| *z < contact_z - 0.5)
        .max_by(f64::total_cmp)
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
        SupportShape::Tapered => (trunk_radius * 1.35).min(base_radius),
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
    fn creates_closed_tapered_pillar() {
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
}
