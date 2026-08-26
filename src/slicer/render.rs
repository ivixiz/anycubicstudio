use super::mesh::{Mesh, Triangle, Vec3};

pub(crate) struct RenderedImage {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

#[derive(Clone, Copy)]
struct CameraPreset {
    width: u32,
    height: u32,
    view: Vec3,
    background: [u8; 4],
    model: [u8; 3],
    support: [u8; 3],
    fill: f64,
}

pub(crate) fn render_previews(model: &Mesh, supports: &Mesh) -> Vec<RenderedImage> {
    [
        CameraPreset {
            width: 224,
            height: 168,
            view: Vec3 {
                x: 1.0,
                y: -1.25,
                z: 0.85,
            },
            background: [2, 72, 100, 255],
            model: [45, 190, 186],
            support: [28, 91, 205],
            fill: 0.68,
        },
        CameraPreset {
            width: 336,
            height: 252,
            view: Vec3 {
                x: 0.15,
                y: -1.0,
                z: 0.18,
            },
            background: [20, 27, 41, 255],
            model: [38, 172, 168],
            support: [37, 91, 205],
            fill: 0.66,
        },
        CameraPreset {
            width: 800,
            height: 600,
            view: Vec3 {
                x: 1.0,
                y: -1.25,
                z: 0.72,
            },
            background: [238, 240, 246, 255],
            model: [45, 190, 186],
            support: [40, 98, 214],
            fill: 0.82,
        },
    ]
    .into_iter()
    .map(|camera| render(model, supports, camera))
    .collect()
}

fn render(model: &Mesh, supports: &Mesh, camera: CameraPreset) -> RenderedImage {
    let view = normalize(camera.view);
    let right = normalize(cross(
        Vec3 {
            x: 0.0,
            y: 0.0,
            z: 1.0,
        },
        view,
    ));
    let up = normalize(cross(view, right));
    let all_points = model
        .triangles
        .iter()
        .chain(&supports.triangles)
        .flat_map(|triangle| triangle.0);
    let bounds = all_points.fold(None, |bounds: Option<[f64; 4]>, point| {
        let projected = [dot(point, right), dot(point, up)];
        Some(match bounds {
            None => [projected[0], projected[1], projected[0], projected[1]],
            Some(bounds) => [
                bounds[0].min(projected[0]),
                bounds[1].min(projected[1]),
                bounds[2].max(projected[0]),
                bounds[3].max(projected[1]),
            ],
        })
    });
    let bounds = bounds.unwrap_or([-1.0, -1.0, 1.0, 1.0]);
    let projected_width = (bounds[2] - bounds[0]).max(1.0e-6);
    let projected_height = (bounds[3] - bounds[1]).max(1.0e-6);
    let scale = (camera.width as f64 * camera.fill / projected_width)
        .min(camera.height as f64 * camera.fill / projected_height);
    let centre = [(bounds[0] + bounds[2]) * 0.5, (bounds[1] + bounds[3]) * 0.5];
    let mut rgba = camera
        .background
        .repeat((camera.width * camera.height) as usize);
    let mut depth = vec![f64::NEG_INFINITY; (camera.width * camera.height) as usize];
    let projection = Projection {
        width: camera.width,
        height: camera.height,
        view,
        right,
        up,
        scale,
        centre,
    };
    for triangle in &supports.triangles {
        rasterize(
            *triangle,
            camera.support,
            &projection,
            &mut rgba,
            &mut depth,
        );
    }
    for triangle in &model.triangles {
        rasterize(*triangle, camera.model, &projection, &mut rgba, &mut depth);
    }
    RenderedImage {
        width: camera.width,
        height: camera.height,
        rgba,
    }
}

struct Projection {
    width: u32,
    height: u32,
    view: Vec3,
    right: Vec3,
    up: Vec3,
    scale: f64,
    centre: [f64; 2],
}

fn rasterize(
    triangle: Triangle,
    colour: [u8; 3],
    projection: &Projection,
    rgba: &mut [u8],
    depth_buffer: &mut [f64],
) {
    let vertices = triangle.0.map(|point| {
        [
            (dot(point, projection.right) - projection.centre[0]) * projection.scale
                + projection.width as f64 * 0.5,
            projection.height as f64 * 0.5
                - (dot(point, projection.up) - projection.centre[1]) * projection.scale,
            dot(point, projection.view),
        ]
    });
    let area = edge(vertices[0], vertices[1], vertices[2][0], vertices[2][1]);
    if area.abs() < 1.0e-9 {
        return;
    }
    let min_x = vertices
        .iter()
        .map(|point| point[0])
        .fold(f64::INFINITY, f64::min)
        .floor()
        .max(0.0) as u32;
    let max_x = vertices
        .iter()
        .map(|point| point[0])
        .fold(f64::NEG_INFINITY, f64::max)
        .ceil()
        .min(projection.width as f64 - 1.0) as u32;
    let min_y = vertices
        .iter()
        .map(|point| point[1])
        .fold(f64::INFINITY, f64::min)
        .floor()
        .max(0.0) as u32;
    let max_y = vertices
        .iter()
        .map(|point| point[1])
        .fold(f64::NEG_INFINITY, f64::max)
        .ceil()
        .min(projection.height as f64 - 1.0) as u32;
    if min_x > max_x || min_y > max_y {
        return;
    }
    let normal = normalize(cross(
        subtract(triangle.0[1], triangle.0[0]),
        subtract(triangle.0[2], triangle.0[0]),
    ));
    let light = normalize(Vec3 {
        x: -0.3,
        y: -0.5,
        z: 1.0,
    });
    let brightness = (0.42 + 0.58 * dot(normal, light).max(0.0)).clamp(0.2, 1.0);
    let shaded = colour.map(|channel| (channel as f64 * brightness).round() as u8);

    for y in min_y..=max_y {
        for x in min_x..=max_x {
            let px = x as f64 + 0.5;
            let py = y as f64 + 0.5;
            let first = edge(vertices[1], vertices[2], px, py) / area;
            let second = edge(vertices[2], vertices[0], px, py) / area;
            let third = 1.0 - first - second;
            if first < -1.0e-8 || second < -1.0e-8 || third < -1.0e-8 {
                continue;
            }
            let z = first * vertices[0][2] + second * vertices[1][2] + third * vertices[2][2];
            let pixel = (y * projection.width + x) as usize;
            if z <= depth_buffer[pixel] {
                continue;
            }
            depth_buffer[pixel] = z;
            let offset = pixel * 4;
            rgba[offset..offset + 3].copy_from_slice(&shaded);
            rgba[offset + 3] = 255;
        }
    }
}

fn edge(first: [f64; 3], second: [f64; 3], x: f64, y: f64) -> f64 {
    (x - first[0]) * (second[1] - first[1]) - (y - first[1]) * (second[0] - first[0])
}

fn subtract(first: Vec3, second: Vec3) -> Vec3 {
    Vec3 {
        x: first.x - second.x,
        y: first.y - second.y,
        z: first.z - second.z,
    }
}

fn cross(first: Vec3, second: Vec3) -> Vec3 {
    Vec3 {
        x: first.y * second.z - first.z * second.y,
        y: first.z * second.x - first.x * second.z,
        z: first.x * second.y - first.y * second.x,
    }
}

fn dot(first: Vec3, second: Vec3) -> f64 {
    first.x * second.x + first.y * second.y + first.z * second.z
}

fn normalize(point: Vec3) -> Vec3 {
    let length = dot(point, point).sqrt();
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_expected_preview_dimensions() {
        let mesh = Mesh {
            triangles: vec![Triangle([
                Vec3 {
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                },
                Vec3 {
                    x: 1.0,
                    y: 0.0,
                    z: 0.0,
                },
                Vec3 {
                    x: 0.0,
                    y: 1.0,
                    z: 1.0,
                },
            ])],
        };
        let previews = render_previews(
            &mesh,
            &Mesh {
                triangles: Vec::new(),
            },
        );
        assert_eq!(previews[0].rgba.len(), 224 * 168 * 4);
        assert_eq!(previews[1].rgba.len(), 336 * 252 * 4);
        assert_eq!(previews[2].rgba.len(), 800 * 600 * 4);
        assert!(
            previews[0]
                .rgba
                .chunks_exact(4)
                .any(|pixel| pixel != [2, 72, 100, 255])
        );
    }
}
