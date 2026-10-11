#include "support_preview.hpp"

#include <QLinearGradient>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPolygonF>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

QColor supportBlue(int alpha = 255) { return QColor(38, 112, 246, alpha); }

void drawMember(QPainter &painter, const QPointF &bottom, const QPointF &top,
                double bottom_radius, double top_radius, bool square) {
    const QPointF direction = top - bottom;
    const double length = std::hypot(direction.x(), direction.y());
    if (length < 0.01)
        return;
    const QPointF normal(-direction.y() / length, direction.x() / length);
    QPolygonF body;
    body << bottom + normal * bottom_radius << top + normal * top_radius
         << top - normal * top_radius << bottom - normal * bottom_radius;
    QLinearGradient gradient(body.boundingRect().topLeft(), body.boundingRect().topRight());
    gradient.setColorAt(0.0, QColor(18, 67, 175));
    gradient.setColorAt(0.48, QColor(54, 137, 255));
    gradient.setColorAt(1.0, QColor(11, 47, 133));
    painter.setPen(QPen(QColor(78, 161, 255), 1.0));
    painter.setBrush(gradient);
    painter.drawPolygon(body);
    if (!square) {
        painter.setPen(QPen(QColor(111, 185, 255, 150), 0.8));
        painter.drawLine(bottom, top);
    }
}

QPolygonF facetedDisc(const QPointF &centre, double radius_x, double radius_y,
                      int segments) {
    QPolygonF polygon;
    const int visible_segments = std::clamp(segments, 6, 32);
    polygon.reserve(visible_segments);
    for (int index = 0; index < visible_segments; ++index) {
        const double angle = 2.0 * M_PI * index / visible_segments;
        polygon << QPointF(centre.x() + std::cos(angle) * radius_x,
                           centre.y() + std::sin(angle) * radius_y);
    }
    return polygon;
}

void drawDisc(QPainter &painter, const QPointF &centre, double radius, int segments,
              bool square) {
    painter.setPen(QPen(QColor(94, 175, 255), 1.0));
    painter.setBrush(supportBlue());
    if (square) {
        painter.drawRect(QRectF(centre.x() - radius, centre.y() - radius * 0.42,
                                radius * 2.0, radius * 0.84));
    } else {
        painter.drawPolygon(facetedDisc(centre, radius, std::max(1.5, radius * 0.32),
                                        segments));
    }
}

} // namespace

SupportPreview::SupportPreview(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
    setObjectName("supportShapePreview");
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(270, 330);
    hide();
}

void SupportPreview::setSettings(const SupportSettings &settings) {
    m_settings = settings;
    update();
}

void SupportPreview::setFocusedParameter(const QString &parameter) {
    if (m_focused_parameter == parameter)
        return;
    m_focused_parameter = parameter;
    update();
}

void SupportPreview::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(76, 99, 132), 1.0));
    painter.setBrush(QColor(14, 20, 31, 247));
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);

    painter.setPen(QColor(226, 234, 246));
    QFont title_font = painter.font();
    title_font.setBold(true);
    painter.setFont(title_font);
    painter.drawText(QRectF(14, 10, width() - 28, 21), Qt::AlignLeft | Qt::AlignVCenter,
                     "Support preview");
    painter.setFont(QFont());
    painter.setPen(QColor(142, 157, 180));
    painter.drawText(QRectF(14, 31, width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter,
                     m_focused_parameter);

    const bool branch = m_settings.shape.compare("branch", Qt::CaseInsensitive) == 0;
    const double display_trunk_diameter =
        branch ? std::min(m_settings.branch_max_trunk,
                          m_settings.trunk * std::sqrt(std::max(1, m_settings.branch_max_children)))
               : m_settings.trunk;
    const double max_diameter = std::max({m_settings.tip, display_trunk_diameter,
                                          m_settings.base, 0.05});
    const double diameter_scale = std::clamp(44.0 / max_diameter, 2.0, 13.0);
    const double tip_radius = std::clamp(m_settings.tip * diameter_scale * 0.5, 1.2, 34.0);
    const double trunk_radius =
        std::clamp(display_trunk_diameter * diameter_scale * 0.5, 1.8, 38.0);
    const double branch_radius =
        std::clamp(m_settings.trunk * diameter_scale * 0.5, 1.8, 30.0);
    const double base_radius =
        std::clamp(m_settings.base * diameter_scale * 0.5, 3.0, 48.0);
    const double base_pixels = std::clamp(m_settings.base_height * 10.0, 5.0, 30.0);
    const double tip_pixels = std::clamp(m_settings.tip_height * 11.0, 7.0, 40.0);
    const double penetration_pixels = std::clamp(m_settings.penetration * 18.0, 0.0, 15.0);
    const bool square = m_settings.shape.compare("square", Qt::CaseInsensitive) == 0;
    const bool tapered = m_settings.shape.compare("tapered", Qt::CaseInsensitive) == 0;
    const int segments = m_settings.segments;

    const double centre_x = width() * 0.5;
    const double bed_y = 272.0;
    const double plane_y = 72.0;
    const double contact_y = plane_y + 5.0 - penetration_pixels;
    const double base_top_y = bed_y - base_pixels;

    painter.save();
    if (!m_settings.enabled)
        painter.setOpacity(0.38);

    // Build plate and the support are drawn before the hanging surface. This
    // makes penetration visible through the translucent plane.
    painter.setPen(QPen(QColor(87, 108, 137), 1.0));
    painter.drawLine(QPointF(38, bed_y + 2), QPointF(width() - 38, bed_y + 2));
    if (branch) {
        const double zone_pixels = std::clamp(m_settings.branch_max_distance * 2.2, 24.0, 105.0);
        painter.setPen(QPen(QColor(105, 135, 174, 130), 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(centre_x, bed_y + 1), zone_pixels, zone_pixels * 0.18);
    }
    drawMember(painter, QPointF(centre_x, bed_y), QPointF(centre_x, base_top_y),
               base_radius, trunk_radius, square);
    drawDisc(painter, QPointF(centre_x, bed_y), base_radius, segments, square);

    if (branch) {
        const int child_count = std::clamp(m_settings.branch_max_children, 1, 5);
        const double spread = std::clamp(16.0 + m_settings.spacing * 5.0, 20.0, 92.0);
        const double maximum_dx = child_count == 1 ? 0.0 : spread * 0.5;
        const double tangent = std::max(0.05, std::tan(qDegreesToRadians(m_settings.angle)));
        const double joint_y = std::clamp(contact_y + std::max(38.0, maximum_dx / tangent),
                                          contact_y + 28.0, base_top_y - 18.0);
        const QPointF joint(centre_x, joint_y);
        drawMember(painter, QPointF(centre_x, base_top_y), joint, trunk_radius,
                   std::max(branch_radius, trunk_radius * 0.72), false);
        for (int child = 0; child < child_count; ++child) {
            const double fraction = child_count == 1
                                        ? 0.0
                                        : child / double(child_count - 1) - 0.5;
            const QPointF contact(centre_x + fraction * spread, contact_y);
            const QPointF tip_start = contact + (joint - contact) *
                                                    std::clamp(tip_pixels /
                                                                   std::max(1.0, QLineF(contact, joint).length()),
                                                               0.08, 0.55);
            drawMember(painter, joint, tip_start, branch_radius,
                       std::max(tip_radius * 1.45, branch_radius * 0.72), false);
            drawMember(painter, tip_start, contact,
                       std::max(tip_radius * 1.45, branch_radius * 0.72), tip_radius,
                       false);
            drawDisc(painter, contact, tip_radius, segments, false);
        }
        drawDisc(painter, joint, std::max(branch_radius, trunk_radius * 0.78), segments,
                 false);
    } else {
        const QPointF tip_start(centre_x, contact_y + tip_pixels);
        const double lower_radius = tapered ? trunk_radius * 1.25 : trunk_radius;
        const double upper_radius = tapered ? std::max(tip_radius * 1.3, trunk_radius * 0.58)
                                             : trunk_radius;
        drawMember(painter, QPointF(centre_x, base_top_y), tip_start, lower_radius,
                   upper_radius, square);
        drawMember(painter, tip_start, QPointF(centre_x, contact_y),
                   std::max(tip_radius * 1.45, upper_radius), tip_radius, square);
        drawDisc(painter, QPointF(centre_x, contact_y), tip_radius, segments, square);
    }

    QPolygonF plane;
    plane << QPointF(34, plane_y) << QPointF(width() - 51, plane_y)
          << QPointF(width() - 25, plane_y - 17) << QPointF(60, plane_y - 17);
    painter.setPen(QPen(QColor(80, 232, 206), 1.2));
    painter.setBrush(QColor(39, 184, 166, 105));
    painter.drawPolygon(plane);
    // Density is previewed as cheap contact markers only. Rendering dozens of
    // complete support meshes here would obscure the one explanatory tree and
    // would make high-density edits unnecessarily expensive.
    const int density_points = std::clamp(int(std::lround(m_settings.density * 0.9)), 1, 24);
    painter.setPen(QPen(QColor(86, 255, 225, 190), 0.8));
    painter.setBrush(QColor(38, 218, 192, 105));
    for (int index = 0; index < density_points; ++index) {
        const int column = index % 8;
        const int row = index / 8;
        const double x = 55.0 + column * (width() - 110.0) / 7.0 + (row % 2) * 3.0;
        const double y = plane_y - 8.0 - row * 5.0;
        painter.drawEllipse(QPointF(x, y), 2.2, 1.1);
    }
    painter.restore();

    if (!m_settings.enabled) {
        painter.setPen(QColor(255, 192, 92));
        painter.drawText(QRectF(14, 282, width() - 28, 18), Qt::AlignCenter,
                         "Supports disabled");
    }
    painter.setPen(QColor(139, 156, 180));
    const QString summary = branch
                                ? QString("branch · %1/cm² · ≤%2 arms · shared Ø≤%3 mm\n"
                                          "tip %4 · arm %5 mm · %6 segments")
                                      .arg(m_settings.density, 0, 'f', 1)
                                      .arg(m_settings.branch_max_children)
                                      .arg(m_settings.branch_max_trunk, 0, 'f', 2)
                                      .arg(m_settings.tip, 0, 'f', 2)
                                      .arg(m_settings.trunk, 0, 'f', 2)
                                      .arg(m_settings.segments)
                                : QString("%1 · %2/cm² · tip %3 · trunk %4 mm\n"
                                          "height %5–%6 mm · %7 segments")
                                      .arg(m_settings.shape)
                                      .arg(m_settings.density, 0, 'f', 1)
                                      .arg(m_settings.tip, 0, 'f', 2)
                                      .arg(m_settings.trunk, 0, 'f', 2)
                                      .arg(m_settings.minimum_height, 0, 'f', 1)
                                      .arg(m_settings.maximum_height, 0, 'f', 1)
                                      .arg(m_settings.segments);
    painter.drawText(QRectF(12, 296, width() - 24, 30), Qt::AlignCenter, summary);
}
