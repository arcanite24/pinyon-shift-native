#include "drive/contact.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace drive {
namespace {

// A body's 3D oriented bounding box: centre, orthonormal 3D axes u[0..2]
// (right, up, forward), half-extents e[0..2] (hw, hh, hl), world vertical
// half-extent half_h, height band [y0, y1], and horizontal forward/right.
struct Box {
  Vec3 c;
  Vec3 u[3];
  double e[3];
  double half_h;
  double fx, fz, rx, rz, hl, hw, y0, y1;
};

Box box_of(const Vehicle& v) {
  const auto& p = v.params();
  const Vec3 local{0, p.sill_height + 0.5 * p.size.y, p.centre_of_mass.z};
  Box b;
  b.c = v.position() + rotate(v.orientation(), local);
  b.u[0] = rotate(v.orientation(), {1, 0, 0});
  b.u[1] = rotate(v.orientation(), {0, 1, 0});
  b.u[2] = rotate(v.orientation(), {0, 0, 1});
  b.e[0] = 0.5 * p.size.x;
  b.e[1] = 0.5 * p.size.y;
  b.e[2] = 0.5 * p.size.z;
  b.half_h = std::fabs(b.u[0].y) * b.e[0] + std::fabs(b.u[1].y) * b.e[1] + std::fabs(b.u[2].y) * b.e[2];
  const Vec3& f = b.u[2];
  const double l = std::max(1e-6, std::hypot(f.x, f.z));
  b.fx = f.x / l, b.fz = f.z / l;
  b.rx = b.fz, b.rz = -b.fx;  // the right of a direction (x, z) is (z, -x)
  b.hl = b.e[2], b.hw = b.e[0];
  b.y0 = b.c.y - b.half_h, b.y1 = b.c.y + b.half_h;
  return b;
}

double obb_radius_on_axis(const Box& b, const Vec3& axis) {
  return b.e[0] * std::fabs(dot(b.u[0], axis)) + b.e[1] * std::fabs(dot(b.u[1], axis)) +
         b.e[2] * std::fabs(dot(b.u[2], axis));
}

bool obb_overlap_3d(const Box& a, const Box& b) {
  const Vec3 d = b.c - a.c;
  auto separated = [&](const Vec3& axis) {
    const double len2 = dot(axis, axis);
    if (len2 < 1e-10) return false;
    const double reach = obb_radius_on_axis(a, axis) + obb_radius_on_axis(b, axis);
    return std::fabs(dot(d, axis)) >= reach;
  };
  for (int i = 0; i < 3; ++i) {
    if (separated(a.u[i])) return false;
    if (separated(b.u[i])) return false;
  }
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      if (separated(cross(a.u[i], b.u[j]))) return false;
    }
  }
  return true;
}

std::array<std::array<double, 2>, 4> corners(const Box& b) {
  std::array<std::array<double, 2>, 4> out;
  int k = 0;
  for (double sl : {-1.0, 1.0})
    for (double sw : {-1.0, 1.0})
      out[size_t(k++)] = {b.c.x + b.fx * b.hl * sl + b.rx * b.hw * sw, b.c.z + b.fz * b.hl * sl + b.rz * b.hw * sw};
  return out;
}

bool inside(const Box& b, double x, double z) {
  const double dx = x - b.c.x, dz = z - b.c.z;
  return std::fabs(dx * b.fx + dz * b.fz) <= b.hl && std::fabs(dx * b.rx + dz * b.rz) <= b.hw;
}

}  // namespace

bool car_contact(Vehicle& a, double scale_a, Vehicle& b, double scale_b, const ContactOptions& o, double* impulse) {
  if (impulse) *impulse = 0;
  auto finite_vec = [](const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
  if (!finite_vec(a.position()) || !finite_vec(b.position()) || !finite_vec(a.velocity()) || !finite_vec(b.velocity()))
    return false;
  const Box A = box_of(a), B = box_of(b);
  if (!finite_vec(A.c) || !finite_vec(B.c) || !std::isfinite(A.fx) || !std::isfinite(B.fx)) return false;
  // Vertical Y extent reject & full 3D OBB SAT overlap test (prevents phantom
  // mid-air collisions when one car jumps over another).
  if (std::fabs(A.c.y - B.c.y) >= A.half_h + B.half_h) return false;
  const double sep_dist = std::hypot(A.c.x - B.c.x, A.c.z - B.c.z);
  if (!std::isfinite(sep_dist) || sep_dist > A.hl + A.hw + B.hl + B.hw) return false;
  if (!obb_overlap_3d(A, B)) return false;
  // Where B stood from A one step before (the relative travel undone).
  const Vec3 va = a.velocity(), vb = b.velocity();
  const double px = (B.c.x - A.c.x) - (vb.x - va.x) * Vehicle::kStep;
  const double py = (B.c.y - A.c.y) - (vb.y - va.y) * Vehicle::kStep;
  const double pz = (B.c.z - A.c.z) - (vb.z - va.z) * Vehicle::kStep;
  // Separating axes: both boxes' forward and right. Overlapping on all of
  // them is a contact; the normal is the axis of least overlap a step ago,
  // signed by the side B was on then.
  const double axes[4][2] = {{A.fx, A.fz}, {A.rx, A.rz}, {B.fx, B.fz}, {B.rx, B.rz}};
  double best_prev = 1e30, depth = 0, nx = 0, ny = 0, nz = 0;
  for (const auto& ax : axes) {
    auto extent = [&](const Box& q) {
      return q.hl * std::fabs(q.fx * ax[0] + q.fz * ax[1]) + q.hw * std::fabs(q.rx * ax[0] + q.rz * ax[1]);
    };
    const double reach = extent(A) + extent(B);
    const double dist = (B.c.x - A.c.x) * ax[0] + (B.c.z - A.c.z) * ax[1];
    if (reach - std::fabs(dist) <= 0) return false;
    const double prev = px * ax[0] + pz * ax[1];
    const double overlap_prev = reach - std::fabs(prev);
    if (overlap_prev < best_prev) {
      best_prev = overlap_prev;
      const double s = prev < 0 ? -1 : 1;
      nx = ax[0] * s, nz = ax[1] * s;  // from A towards B
      depth = reach - dist * s;         // how far B must go along the normal to clear A
    }
  }
  // Vertical roof-landing check: if the two boxes were separated along Y a step
  // ago (or least-overlapping along Y), resolve vertically instead of snapping sideways.
  const double reach_y = A.half_h + B.half_h;
  const double overlap_prev_y = reach_y - std::fabs(py);
  if (overlap_prev_y < best_prev && overlap_prev_y <= 0.05) {
    const double sy = py < 0 ? -1.0 : 1.0;
    nx = 0.0;
    ny = sy;
    nz = 0.0;
    depth = reach_y - (B.c.y - A.c.y) * sy;
  }
  // The contact point: the corners of each box inside the other.
  double cx = 0, cz = 0;
  int n = 0;
  for (const auto& c : corners(B))
    if (inside(A, c[0], c[1])) cx += c[0], cz += c[1], ++n;
  for (const auto& c : corners(A))
    if (inside(B, c[0], c[1])) cx += c[0], cz += c[1], ++n;
  if (n == 0) cx = 0.5 * (A.c.x + B.c.x), cz = 0.5 * (A.c.z + B.c.z), n = 1;
  const Vec3 point{cx / n, 0.5 * (std::max(A.y0, B.y0) + std::min(A.y1, B.y1)), cz / n};
  const Vec3 normal{nx, ny, nz};
  const double ia = a.inverse_mass_along(point, normal, scale_a), ib = b.inverse_mass_along(point, normal, scale_b);
  if (!std::isfinite(depth) || !std::isfinite(ia) || !std::isfinite(ib) || ia + ib <= 1e-12) return false;
  // Separation, shared by inverse mass.
  const double ma = 1 / a.params().mass, mb = 1 / b.params().mass;
  const double push = std::clamp(depth - 0.01, 0.0, 2.0);
  a.translate(normal * (-push * ma / (ma + mb)));
  b.translate(normal * (push * mb / (ma + mb)));
  const Vec3 rel = b.point_velocity(point) - a.point_velocity(point);
  const double vn = dot(rel, normal);
  if (!std::isfinite(vn) || vn >= 0) return true;
  const double j = -(1 + o.restitution) * vn / (ia + ib);
  if (!std::isfinite(j)) return true;
  a.apply_impulse(point, normal * -j, scale_a);
  b.apply_impulse(point, normal * j, scale_b);
  // Friction along the contact, at most friction x j.
  Vec3 t = rel - normal * vn;
  if (ny == 0.0) t.y = 0;
  const double tl = std::sqrt(dot(t, t));
  if (tl > 1e-6 && std::isfinite(tl)) {
    t = t * (1 / tl);
    const double it = a.inverse_mass_along(point, t, scale_a) + b.inverse_mass_along(point, t, scale_b);
    if (std::isfinite(it) && it > 1e-12) {
      const double jt = std::min(o.friction * j, tl / it);
      if (std::isfinite(jt)) {
        a.apply_impulse(point, t * jt, scale_a);
        b.apply_impulse(point, t * -jt, scale_b);
      }
    }
  }
  if (impulse) *impulse = j;
  return true;
}

int resolve_contacts(std::span<const ContactBody> bodies, const ContactOptions& o, double* largest) {
  int n = 0;
  for (size_t i = 0; i < bodies.size(); ++i)
    for (size_t k = i + 1; k < bodies.size(); ++k) {
      const ContactBody &p = bodies[i], &q = bodies[k];
      if (!p.v || !q.v || (p.group > 0 && p.group == q.group)) continue;
      const Vec3 d = p.v->position() - q.v->position();
      const double r = p.v->params().size.z + q.v->params().size.z;  // generous 3D sphere
      const double d2 = d.x * d.x + d.y * d.y + d.z * d.z;
      if (!(d2 <= r * r)) continue;
      double j = 0;
      const double scale_p =
          (p.angular_scale_vs_traffic >= 0 && q.role == ContactRole::kTraffic) ? p.angular_scale_vs_traffic : p.angular_scale;
      const double scale_q =
          (q.angular_scale_vs_traffic >= 0 && p.role == ContactRole::kTraffic) ? q.angular_scale_vs_traffic : q.angular_scale;
      if (car_contact(*p.v, scale_p, *q.v, scale_q, o, &j)) {
        ++n;
        if (largest) *largest = std::max(*largest, j);
      }
    }
  return n;
}

}  // namespace drive
