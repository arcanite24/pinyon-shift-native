// Shared GLSL definitions, BDA vertex fetch, surface unpacking, automotive BSDF,
// analytical TOD sky, and sampling helpers for RT-2 (Hybrid RT) & RT-3/4 (Path Tracing).
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require

const float kPi = 3.14159265359;
const float kInvPi = 0.31830988618;

// 8-bit TLAS visibility masks (matching rt_logic.h).
const uint kMaskPlayerCar       = 0x01u;
const uint kMaskTrafficCar      = 0x02u;
const uint kMaskWorldOpaque     = 0x04u;
const uint kMaskFoliageAlpha    = 0x08u;
const uint kMaskGlass           = 0x10u;
const uint kMaskEmissive        = 0x20u;
const uint kMaskWater           = 0x40u;
const uint kMaskCockpitInterior = 0x80u;
const uint kMaskShadowCasters   = kMaskPlayerCar | kMaskTrafficCar | kMaskWorldOpaque | kMaskFoliageAlpha;
const uint kMaskReflectionHits  = kMaskPlayerCar | kMaskTrafficCar | kMaskWorldOpaque | kMaskFoliageAlpha | kMaskGlass | kMaskEmissive | kMaskWater;
const uint kMaskPrimaryHits     = 0xFFu;

// Geometry flag bits (matching rt_scene.h).
const uint kRtGeomFlagCarVertex   = 1u << 0;
const uint kRtGeomFlagMetallic    = 1u << 1;
const uint kRtGeomFlagClearcoat   = 1u << 2;
const uint kRtGeomFlagGlass       = 1u << 3;
const uint kRtGeomFlagEmissive    = 1u << 4;
const uint kRtGeomFlagWater       = 1u << 5;
const uint kRtGeomFlagRoad        = 1u << 6;
const uint kRtGeomFlagBindlessTex = 1u << 7;
const uint kRtGeomFlagCutout      = 1u << 8;
const uint kRtGeomFlagChrome      = 1u << 9;
const uint kRtGeomFlagVertexBlend = 1u << 10;
const uint kRtGeomFlagTreeCross   = 1u << 11;

struct RtGeomInfo {
  uint64_t vertex_bda;
  uint64_t index_bda;
  uint material_class;
  uint flags;
  uint tex_albedo;
  uint tex_aux;
  vec4 tint;       // rgb linear base color, a roughness
  vec4 uv0_xform;  // offset_u, scale_u, offset_v, scale_v
};

struct RtInstanceInfo {
  uint geom_offset;
  uint geom_count;
  uint kind;
  uint car_inst_idx;
  vec4 model_row0;
  vec4 model_row1;
  vec4 model_row2;
};

struct RtLightGpu {
  vec4 pos_type;     // xyz world pos (or dir for sun), w: 0=sun, 1=headlight, 2=taillight, 3=world glow
  vec4 dir_range;    // xyz unit dir, w = range
  vec4 color_power;  // rgb linear radiance, w = cone outer cos
  vec4 extra;        // x = cone inner cos, y = radius, z = owner car id, w = flags
};

struct RtReservoir {
  uint light_idx;
  float weight_sum;
  float W;
  uint M;
};

// Tightly packed BDA vertex buffers matching C++ WorldVertex (48 B) and cars::bake::Vertex (32 B).
struct WorldVertRaw {
  float px, py, pz;
  int8_t nx, ny, nz, nw;
  float u0, v0;
  float u1, v1;
  uint8_t cr, cg, cb, ca;
  int16_t cx, cy, cz, cw;
  uint16_t u2, v2;
};

struct CarVertRaw {
  float px, py, pz;
  int16_t qx, qy, qz, qw;
  uint16_t u0, v0;
  uint16_t u1, v1;
  uint8_t l0, l1, l2, l3;
};

layout(buffer_reference, scalar) readonly buffer WorldVertBuf { WorldVertRaw v[]; };
layout(buffer_reference, scalar) readonly buffer CarVertBuf   { CarVertRaw v[]; };
layout(buffer_reference, scalar) readonly buffer IndexBuf     { uint idx[]; };

// Shared descriptor set 0 (all RT/PT compute shaders).
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(std430, set = 0, binding = 1) readonly buffer InstInfos { RtInstanceInfo inst_infos[]; };
layout(std430, set = 0, binding = 2) readonly buffer GeomInfos { RtGeomInfo geom_infos[]; };
layout(std430, set = 0, binding = 3) readonly buffer LightsBuf { RtLightGpu lights[]; };
layout(set = 0, binding = 4) uniform RtFrameUbo {
  mat4 view_proj;
  mat4 inv_view_proj;
  mat4 prev_view_proj;
  vec4 eye_frame;         // xyz eye, w frame_index
  vec4 sun_dir_elev;      // xyz unit sun dir, w sun_elev_vis
  vec4 sun_color_night;   // rgb sun color, w night_factor
  vec4 sky_color_exp;     // rgb sky top color, w exposure
  vec4 ground_color_revz; // rgb ground color, w reverse_z
  vec4 fog_color_start;   // rgb fog color, w fog_start
  vec4 fog_params;        // x fog_density, y near, z far, w accum_count
  vec4 env_basis_x;       // xyz game X, w time_sec
  vec4 env_basis_y;       // xyz game Y, w rt_mode (1=hybrid, 2=pt, 3=photo_pt)
  vec4 env_basis_z;       // xyz game Z, w rt_bounces
  vec4 rt_params;         // x rt_spp, y light_count, z width, w height
  vec4 paint_two_tone;    // rgb secondary color, w power
  vec4 paint_finish;      // x finish, y scale, z bias, w window_tint
} ubo;
layout(set = 0, binding = 5) uniform samplerCube dyn_env;
layout(set = 0, binding = 6) uniform sampler2D depth_tex;
layout(set = 0, binding = 7) uniform sampler2D motion_tex;
layout(set = 0, binding = 8,  rgba16f) uniform image2D hdr_img;
layout(set = 0, binding = 9,  rgba16f) uniform image2D rt_gbuf_norm;
layout(set = 0, binding = 10, rgba16f) uniform image2D rt_gbuf_albedo;
layout(set = 0, binding = 11, rgba16f) uniform image2D rt_refl_img;
layout(set = 0, binding = 12, rg16f)   uniform image2D rt_shad_ao_img;
layout(set = 0, binding = 13, rgba16f) uniform image2D rt_pt_img;
layout(set = 0, binding = 14, rgba16f) uniform image2D rt_hist_prev;
layout(set = 0, binding = 15, rgba16f) uniform image2D rt_hist_curr;
layout(set = 0, binding = 16, rgba16f) uniform image2D rt_svgf_tmp;
layout(set = 0, binding = 17, rgba16f) uniform image2D rt_moments_prev;
layout(set = 0, binding = 18, rgba16f) uniform image2D rt_moments_curr;
layout(std430, set = 0, binding = 19) readonly  buffer ReservoirsPrev { RtReservoir res_prev[]; };
layout(std430, set = 0, binding = 20) writeonly buffer ReservoirsCurr { RtReservoir res_curr[]; };

// Shared descriptor set 1 (WorldGpu split bindless textures).
layout(set = 1, binding = 0) uniform sampler world_sampler;
layout(set = 1, binding = 1) uniform texture2D world_textures[];

layout(push_constant) uniform RtPush {
  ivec4 pc;  // x = pass_param (step_size / mode), y = ping_pong, z = flags, w = spare
} rt_pc;

// ---- Fast PCG spatiotemporal RNG ----
uint pcg_hash(uint v) {
  uint state = v * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

uint init_rng(uvec2 px, uint frame_idx, uint salt) {
  return pcg_hash(px.x + pcg_hash(px.y + pcg_hash(frame_idx * 1664525u + salt)));
}

float rand01(inout uint rng) {
  rng = pcg_hash(rng);
  return float(rng & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

vec2 rand2(inout uint rng) {
  return vec2(rand01(rng), rand01(rng));
}

float luma(vec3 c) {
  return dot(max(c, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
}

vec3 safe_normalize(vec3 v, vec3 fallback) {
  float l2 = dot(v, v);
  return l2 > 1e-10 ? v * inversesqrt(l2) : fallback;
}

void make_onb(vec3 n, out vec3 t, out vec3 b) {
  vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  t = normalize(cross(up, n));
  b = cross(n, t);
}

vec3 sample_cosine_hemisphere(vec3 n, vec2 u) {
  float r = sqrt(clamp(u.x, 0.0, 1.0));
  float phi = 2.0 * kPi * u.y;
  float x = r * cos(phi);
  float y = r * sin(phi);
  float z = sqrt(max(0.0, 1.0 - u.x));
  vec3 t, b;
  make_onb(n, t, b);
  return normalize(t * x + b * y + n * z);
}

// Heitz (2018) Sampling the GGX Distribution of Visible Normals (VNDF).
vec3 sample_ggx_vndf(vec3 N, vec3 V, float roughness, vec2 u) {
  float a = max(roughness * roughness, 0.0025);
  vec3 T, B;
  make_onb(N, T, B);
  vec3 Ve = vec3(dot(V, T), dot(V, B), max(dot(V, N), 1e-4));
  vec3 Vh = normalize(vec3(a * Ve.x, a * Ve.y, Ve.z));
  float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
  vec3 T1 = lensq > 1e-7 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
  vec3 T2 = cross(Vh, T1);
  float r = sqrt(clamp(u.x, 0.0, 0.9999));
  float phi = 2.0 * kPi * u.y;
  float t1 = r * cos(phi);
  float t2 = r * sin(phi);
  float s = 0.5 * (1.0 + Vh.z);
  t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
  vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
  vec3 Ne = normalize(vec3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
  return normalize(T * Ne.x + B * Ne.y + N * Ne.z);
}

vec3 fresnel_schlick(vec3 F0, float cos_theta) {
  float f = pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
  return F0 + (vec3(1.0) - F0) * f;
}

float ggx_d(float ndh, float a) {
  float a2 = a * a;
  float d = (ndh * ndh) * (a2 - 1.0) + 1.0;
  return a2 / max(kPi * d * d, 1e-6);
}

float smith_g1(float ndx, float a) {
  float a2 = a * a;
  return 2.0 * ndx / max(ndx + sqrt(a2 + (1.0 - a2) * ndx * ndx), 1e-5);
}

// Reconstructs world position and ray direction from pixel UV + depth buffer.
vec3 world_pos_from_depth(vec2 uv, float z) {
  vec4 clip = vec4(uv * 2.0 - 1.0, z, 1.0);
  vec4 w = ubo.inv_view_proj * clip;
  return w.xyz / max(abs(w.w), 1e-6) * sign(w.w == 0.0 ? 1.0 : w.w);
}

vec3 primary_ray_dir(vec2 uv) {
  float far_z = ubo.ground_color_revz.w > 0.5 ? 0.001 : 0.999;
  vec3 p = world_pos_from_depth(uv, far_z);
  return normalize(p - ubo.eye_frame.xyz);
}

bool is_sky_depth_val(float z) {
  return ubo.ground_color_revz.w > 0.5 ? (z <= 1e-4) : (z >= 0.9999);
}

float linearize_depth(float z) {
  float near_p = max(ubo.fog_params.y, 0.05);
  if (ubo.ground_color_revz.w > 0.5) {
    return z <= 1e-5 ? 1e5 : near_p / z;
  }
  float far_p = max(ubo.fog_params.z, 100.0);
  return (near_p * far_p) / max(far_p - z * (far_p - near_p), 1e-4);
}

// Analytical Colorado Time-of-Day sky + sun disc + dyn_env fallback (physically calibrated for PT).
vec3 eval_tod_sky(vec3 dir, bool include_sun_disc) {
  vec3 up = safe_normalize(ubo.env_basis_y.xyz, vec3(0.0, 1.0, 0.0));
  vec3 sun_d = safe_normalize(ubo.sun_dir_elev.xyz, vec3(0.38, 0.87, 0.29));
  float up_dot = clamp(dot(dir, up), -1.0, 1.0);
  float night = clamp(ubo.sun_color_night.w, 0.0, 1.0);
  float sunset_warmth = clamp(1.0 - max(sun_d.y, 0.0) * 2.4, 0.0, 1.0) * (1.0 - night);

  vec3 zenith = max(ubo.sky_color_exp.rgb * 3.2, vec3(0.04, 0.08, 0.18) * (1.0 - night * 0.85));
  vec3 horizon = mix(ubo.fog_color_start.rgb * 2.6 + vec3(0.35, 0.42, 0.48) * (1.0 - night),
                     ubo.sun_color_night.rgb * 1.45 + vec3(0.30, 0.14, 0.05), sunset_warmth);
  vec3 ground = max(ubo.ground_color_revz.rgb * 1.35, vec3(0.06, 0.05, 0.04));

  vec3 sky = up_dot >= 0.0
                 ? mix(horizon, zenith, pow(clamp(up_dot, 0.0, 1.0), 0.50))
                 : mix(horizon, ground, pow(clamp(-up_dot, 0.0, 1.0), 0.45));

  // Mie forward solar glow near horizon/sunset.
  float sdot = max(dot(dir, sun_d), 0.0);
  float sun_vis = clamp(ubo.sun_dir_elev.w, 0.0, 1.0);
  sky += ubo.sun_color_night.rgb * (pow(sdot, 6.0) * 0.55 + pow(sdot, 48.0) * 1.8) * sun_vis;
  if (include_sun_disc && sun_vis > 0.01) {
    float disc = smoothstep(0.99978, 0.99993, sdot);
    sky += ubo.sun_color_night.rgb * (disc * 24.0 * sun_vis);
  }
  // Blend with live dyn_env cubemap for horizon clouds & distant terrain continuity.
  vec3 env_sample = textureLod(dyn_env, dir, 1.0).rgb;
  if (dot(env_sample, env_sample) > 1e-4) {
    sky = mix(sky, min(env_sample * 1.45, vec3(14.0)), 0.35);
  }
  return max(sky, vec3(0.0));
}

vec3 apply_rt_haze(vec3 col, float dist, vec3 ray_dir) {
  float fog_start = max(ubo.fog_color_start.w, 120.0);
  float density = max(ubo.fog_params.x, 0.00025);
  float f = 1.0 - exp(-max(dist - fog_start * 0.25, 0.0) * density * 0.85);
  f = clamp(f, 0.0, 0.78);
  vec3 sun_d = safe_normalize(ubo.sun_dir_elev.xyz, vec3(0.38, 0.87, 0.29));
  float sdot = max(dot(ray_dir, sun_d), 0.0);
  vec3 fog_c = ubo.fog_color_start.rgb + ubo.sun_color_night.rgb * pow(sdot, 6.0) * 0.22 * ubo.sun_dir_elev.w;
  return mix(col, max(fog_c, vec3(0.0)), f);
}

// Quaternion rotation matching showroom.vert:73-80.
vec3 quat_rotate(vec4 q, vec3 v) {
  return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

struct RtSurface {
  vec3 pos;
  vec3 normal;
  vec3 geom_normal;
  vec3 albedo;
  vec3 emissive;
  float roughness;
  float metallic;
  float clearcoat;
  float occlusion;
  float hit_t;
  uint flags;
  uint material_class;
  uint inst_kind;
  uint car_id;
  bool hit;
};

// Unpacks complete surface attributes from a committed rayQueryEXT triangle hit.
RtSurface unpack_hit_surface(rayQueryEXT rq, vec3 ray_origin, vec3 ray_dir, float ray_cone_lod) {
  RtSurface s;
  s.hit = false;
  s.hit_t = 1e5;
  s.pos = vec3(0.0);
  s.normal = vec3(0.0, 1.0, 0.0);
  s.geom_normal = vec3(0.0, 1.0, 0.0);
  s.albedo = vec3(0.5);
  s.emissive = vec3(0.0);
  s.roughness = 0.5;
  s.metallic = 0.0;
  s.clearcoat = 0.0;
  s.occlusion = 1.0;
  s.flags = 0u;
  s.material_class = 0u;
  s.inst_kind = 0u;
  s.car_id = 0u;

  if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
    return s;
  }
  s.hit = true;
  s.hit_t = rayQueryGetIntersectionTEXT(rq, true);
  s.pos = ray_origin + ray_dir * s.hit_t;

  uint custom = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
  uint info_idx = custom & 0x000FFFFFu;
  uint geom_rel = uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
  uint prim_id = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
  vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
  vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

  RtInstanceInfo inst = inst_infos[info_idx];
  RtGeomInfo g = geom_infos[inst.geom_offset + min(geom_rel, max(inst.geom_count, 1u) - 1u)];
  s.flags = g.flags;
  s.material_class = g.material_class;
  s.inst_kind = inst.kind;
  s.car_id = inst.car_inst_idx;

  mat3 m_rot = mat3(
      vec3(inst.model_row0.x, inst.model_row1.x, inst.model_row2.x),
      vec3(inst.model_row0.y, inst.model_row1.y, inst.model_row2.y),
      vec3(inst.model_row0.z, inst.model_row1.z, inst.model_row2.z));

  IndexBuf ib = IndexBuf(g.index_bda);
  uint i0 = ib.idx[prim_id * 3u + 0u];
  uint i1 = ib.idx[prim_id * 3u + 1u];
  uint i2 = ib.idx[prim_id * 3u + 2u];

  vec3 p0, p1, p2;
  vec3 n_local = vec3(0.0, 1.0, 0.0);
  vec2 uv0 = vec2(0.0);
  vec3 vcol = vec3(1.0);
  float occ = 1.0;

  uint tree_tex_slot = g.tex_albedo;
  if ((g.flags & kRtGeomFlagCarVertex) != 0u) {
    CarVertBuf vb = CarVertBuf(g.vertex_bda);
    CarVertRaw v0 = vb.v[i0];
    CarVertRaw v1 = vb.v[i1];
    CarVertRaw v2 = vb.v[i2];
    p0 = vec3(v0.px, v0.py, v0.pz);
    p1 = vec3(v1.px, v1.py, v1.pz);
    p2 = vec3(v2.px, v2.py, v2.pz);

    vec4 q0 = normalize(vec4(float(v0.qx), float(v0.qy), float(v0.qz), float(v0.qw)));
    vec4 q1 = normalize(vec4(float(v1.qx), float(v1.qy), float(v1.qz), float(v1.qw)));
    vec4 q2 = normalize(vec4(float(v2.qx), float(v2.qy), float(v2.qz), float(v2.qw)));
    vec3 n0 = quat_rotate(q0, vec3(1.0, 0.0, 0.0));
    vec3 n1 = quat_rotate(q1, vec3(1.0, 0.0, 0.0));
    vec3 n2 = quat_rotate(q2, vec3(1.0, 0.0, 0.0));
    n_local = safe_normalize(n0 * w.x + n1 * w.y + n2 * w.z, vec3(0.0, 1.0, 0.0));

    vec2 t0 = vec2(float(v0.u0), float(v0.v0)) * (1.0 / 65535.0);
    vec2 t1 = vec2(float(v1.u0), float(v1.v0)) * (1.0 / 65535.0);
    vec2 t2 = vec2(float(v2.u0), float(v2.v0)) * (1.0 / 65535.0);
    vec2 uvr = t0 * w.x + t1 * w.y + t2 * w.z;
    uv0 = vec2(g.uv0_xform.x + g.uv0_xform.y * uvr.x, g.uv0_xform.z + g.uv0_xform.w * uvr.y);
    occ = clamp((float(v0.l0) * w.x + float(v1.l0) * w.y + float(v2.l0) * w.z) / 100.0, 0.20, 1.0);
  } else {
    WorldVertBuf vb = WorldVertBuf(g.vertex_bda);
    WorldVertRaw v0 = vb.v[i0];
    WorldVertRaw v1 = vb.v[i1];
    WorldVertRaw v2 = vb.v[i2];
    p0 = vec3(v0.px, v0.py, v0.pz);
    p1 = vec3(v1.px, v1.py, v1.pz);
    p2 = vec3(v2.px, v2.py, v2.pz);

    vec3 n0 = vec3(float(v0.nx), float(v0.ny), float(v0.nz)) * (1.0 / 127.0);
    vec3 n1 = vec3(float(v1.nx), float(v1.ny), float(v1.nz)) * (1.0 / 127.0);
    vec3 n2 = vec3(float(v2.nx), float(v2.ny), float(v2.nz)) * (1.0 / 127.0);
    vec3 ns = n0 * w.x + n1 * w.y + n2 * w.z;
    n_local = dot(ns, ns) > 0.01 ? normalize(ns) : safe_normalize(cross(p1 - p0, p2 - p0), vec3(0.0, 1.0, 0.0));
    uv0 = vec2(v0.u0, v0.v0) * w.x + vec2(v1.u0, v1.v0) * w.y + vec2(v2.u0, v2.v0) * w.z;
    vec3 c0 = vec3(float(v0.cr), float(v0.cg), float(v0.cb)) * (1.0 / 255.0);
    vec3 c1 = vec3(float(v1.cr), float(v1.cg), float(v1.cb)) * (1.0 / 255.0);
    vec3 c2 = vec3(float(v2.cr), float(v2.cg), float(v2.cb)) * (1.0 / 255.0);
    vcol = c0 * w.x + c1 * w.y + c2 * w.z;
    if ((g.flags & kRtGeomFlagTreeCross) != 0u && uint(v0.u2) > 0u) {
      tree_tex_slot = uint(v0.u2);
    }
  }

  vec3 gn_local = safe_normalize(cross(p1 - p0, p2 - p0), n_local);
  s.geom_normal = safe_normalize(m_rot * gn_local, vec3(0.0, 1.0, 0.0));
  s.normal = safe_normalize(m_rot * n_local, s.geom_normal);
  if (dot(s.geom_normal, ray_dir) > 0.0) s.geom_normal = -s.geom_normal;
  if (dot(s.normal, s.geom_normal) < 0.0) s.normal = -s.normal;

  s.albedo = g.tint.rgb;
  s.roughness = clamp(g.tint.a, 0.02, 0.98);
  s.metallic = ((g.flags & kRtGeomFlagMetallic) != 0u) ? 0.92 : 0.0;
  s.clearcoat = ((g.flags & kRtGeomFlagClearcoat) != 0u) ? 1.0 : 0.0;
  s.occlusion = occ;

  if ((g.flags & kRtGeomFlagBindlessTex) != 0u && tree_tex_slot > 0u) {
    vec2 uv_a = uv0 * g.uv0_xform.xy;
    vec4 tex = textureLod(sampler2D(world_textures[nonuniformEXT(tree_tex_slot)], world_sampler), uv_a, ray_cone_lod);
    if ((g.flags & kRtGeomFlagVertexBlend) != 0u && g.tex_aux > 0u) {
      vec2 uv_b = uv0 * g.uv0_xform.zw;
      vec4 tex_b = textureLod(sampler2D(world_textures[nonuniformEXT(g.tex_aux)], world_sampler), uv_b, ray_cone_lod);
      tex.rgb = mix(tex.rgb, tex_b.rgb, clamp(vcol.r, 0.0, 1.0));
    }
    if ((g.flags & kRtGeomFlagTreeCross) != 0u) {
      s.albedo = tex.rgb * mix(vec3(1.0), pow(max(vcol, vec3(0.0)), vec3(1.8)), 0.5);
    } else {
      // Textured FH1 roads & terrain use texture RGB directly (vcol holds blend/splat weights, matching world_frag.glsl!).
      s.albedo = tex.rgb;
    }
  } else if ((g.flags & kRtGeomFlagCarVertex) == 0u) {
    s.albedo = g.tint.rgb * pow(max(vcol, vec3(0.0)), vec3(1.8));
  }

  // Water waves normal perturbation for Colorado alpine lakes & rivers.
  if ((g.flags & kRtGeomFlagWater) != 0u) {
    float t = ubo.env_basis_x.w;
    vec3 wp = s.pos;
    float w1 = sin(wp.x * 1.4 + t * 1.7) * cos(wp.z * 1.2 - t * 1.3);
    float w2 = sin(wp.x * 3.1 - wp.z * 2.7 + t * 2.4);
    s.normal = safe_normalize(s.normal + vec3(w1 * 0.025 + w2 * 0.012, 0.0, w2 * 0.022), s.normal);
    s.albedo = vec3(0.03, 0.07, 0.10);
    s.roughness = 0.04;
    s.clearcoat = 1.0;
  }

  // Automotive metallic micro-flake normal perturbation (3D local-space stable so flakes never swim!).
  if ((g.flags & kRtGeomFlagCarVertex) != 0u && (g.flags & kRtGeomFlagMetallic) != 0u && s.material_class == 1u) {
    vec3 lp = p0 * w.x + p1 * w.y + p2 * w.z;
    vec3 cell = floor(lp * 280.0);
    float h1 = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    float h2 = fract(sin(dot(cell, vec3(93.9898, 67.345, 19.191))) * 24634.6345);
    float dist_fade = clamp(1.0 - s.hit_t / 12.0, 0.0, 1.0);
    vec3 ft, fb;
    make_onb(s.normal, ft, fb);
    s.normal = normalize(s.normal + (ft * (h1 - 0.5) + fb * (h2 - 0.5)) * (0.045 * dist_fade));
  }

  // Night emissive car lamps & world lightmaps.
  if ((g.flags & kRtGeomFlagEmissive) != 0u) {
    float night = clamp(ubo.sun_color_night.w, 0.0, 1.0);
    if ((g.flags & kRtGeomFlagCarVertex) != 0u) {
      // tex_aux: 1 = active tail/brake red, 2 = active head/DRL xenon white.
      if (g.tex_aux == 1u) {
        s.emissive = vec3(5.2, 0.14, 0.04) * max(night, 0.72);
      } else if (g.tex_aux == 2u) {
        s.emissive = vec3(5.4, 5.1, 4.5) * max(night, 0.40);
      }
    } else if (g.tex_aux > 0u && (g.flags & kRtGeomFlagVertexBlend) == 0u) {
      vec3 lm = textureLod(sampler2D(world_textures[nonuniformEXT(g.tex_aux)], world_sampler), uv0, ray_cone_lod).rgb;
      s.emissive = lm * (2.5 * night);
    }
  }
  return s;
}

// Evaluates alpha cutout for non-opaque candidate triangle hits (pine tree cross-cards & fences).
bool candidate_passes_alpha(rayQueryEXT rq) {
  uint custom = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false));
  uint info_idx = custom & 0x000FFFFFu;
  uint geom_rel = uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false));
  RtInstanceInfo inst = inst_infos[info_idx];
  RtGeomInfo g = geom_infos[inst.geom_offset + min(geom_rel, max(inst.geom_count, 1u) - 1u)];
  if ((g.flags & kRtGeomFlagCutout) == 0u || (g.flags & kRtGeomFlagBindlessTex) == 0u) {
    return true;  // glass or opaque non-cutout
  }
  uint prim_id = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
  vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, false);
  vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
  IndexBuf ib = IndexBuf(g.index_bda);
  WorldVertBuf vb = WorldVertBuf(g.vertex_bda);
  WorldVertRaw v0 = vb.v[ib.idx[prim_id * 3u + 0u]];
  WorldVertRaw v1 = vb.v[ib.idx[prim_id * 3u + 1u]];
  WorldVertRaw v2 = vb.v[ib.idx[prim_id * 3u + 2u]];
  vec2 uv0 = vec2(v0.u0, v0.v0) * w.x + vec2(v1.u0, v1.v0) * w.y + vec2(v2.u0, v2.v0) * w.z;
  uint slot = ((g.flags & kRtGeomFlagTreeCross) != 0u && uint(v0.u2) > 0u) ? uint(v0.u2) : g.tex_albedo;
  if (slot == 0u) return false;
  float alpha = textureLod(sampler2D(world_textures[nonuniformEXT(slot)], world_sampler), uv0 * g.uv0_xform.xy, 0.5).a;
  return alpha >= 0.42;
}

// Advances a rayQueryEXT with alpha-cutout testing on foliage/fences (capped at 12 alpha tests).
#define PROCEED_ALPHA_TESTED(rq)                                                                        \
  do {                                                                                                  \
    int _alpha_steps = 0;                                                                               \
    while (rayQueryProceedEXT(rq)) {                                                                    \
      if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {   \
        if (candidate_passes_alpha(rq) || _alpha_steps >= 12) {                                         \
          rayQueryConfirmIntersectionEXT(rq);                                                           \
        }                                                                                               \
        ++_alpha_steps;                                                                                 \
      }                                                                                                 \
    }                                                                                                   \
  } while (false)

// Traces a shadow ray against TLAS with crisp alpha-tested foliage shadows (returns 1.0 if visible, 0.0 if occluded).
float trace_visibility(vec3 origin, vec3 dir, float t_min, float t_max, uint mask) {
  rayQueryEXT sq;
  rayQueryInitializeEXT(sq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, mask, origin, t_min, dir, t_max);
  PROCEED_ALPHA_TESTED(sq);
  return rayQueryGetIntersectionTypeEXT(sq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

// Fast direct + hemisphere hit shading for Tier 1 Hybrid RT reflections and secondary bounce fallback.
vec3 shade_hit_fast(RtSurface s, vec3 V, bool test_sun_shadow) {
  vec3 sun_d = safe_normalize(ubo.sun_dir_elev.xyz, vec3(0.38, 0.87, 0.29));
  vec3 up = safe_normalize(ubo.env_basis_y.xyz, vec3(0.0, 1.0, 0.0));
  float ndl = max(dot(s.normal, sun_d), 0.0);
  float sun_vis = clamp(ubo.sun_dir_elev.w, 0.0, 1.0);

  float vis = 1.0;
  if (test_sun_shadow && ndl > 0.005 && sun_vis > 0.01) {
    vec3 orig = s.pos + s.geom_normal * max(0.025, 0.0008 * s.hit_t);
    vis = trace_visibility(orig, sun_d, 0.01, 450.0, kMaskShadowCasters);
  }

  vec3 F0 = mix(vec3(0.04), s.albedo, s.metallic);
  vec3 diff = s.albedo * (1.0 - s.metallic) * kInvPi;
  vec3 H = safe_normalize(sun_d + V, s.normal);
  float ndh = max(dot(s.normal, H), 0.0);
  float ndv = max(dot(s.normal, V), 1e-4);
  float a = max(s.roughness * s.roughness, 0.01);
  vec3 F = fresnel_schlick(F0, max(dot(H, V), 0.0));
  float D = ggx_d(ndh, a);
  float G = smith_g1(ndv, a) * smith_g1(max(ndl, 1e-4), a);
  vec3 spec = (D * G * F) / max(4.0 * ndv * max(ndl, 1e-4), 1e-4);

  // Clearcoat sun highlight on reflected cars.
  if (s.clearcoat > 0.5) {
    float Dc = ggx_d(ndh, 0.035);
    float Fc = 0.047 + 0.953 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
    spec += vec3(Dc * Fc * 0.25);
  }

  vec3 direct = (diff + spec) * ubo.sun_color_night.rgb * (ndl * vis * sun_vis * 2.4);

  // Sky / ground hemisphere ambient.
  float hemi = dot(s.normal, up) * 0.5 + 0.5;
  vec3 amb_col = mix(ubo.ground_color_revz.rgb, ubo.sky_color_exp.rgb * 1.35, hemi);
  vec3 ambient = s.albedo * (1.0 - s.metallic * 0.75) * amb_col * s.occlusion * 1.15;

  // Approximate env reflection for chrome/clearcoat surfaces seen in reflections.
  if (s.metallic > 0.5 || s.clearcoat > 0.5) {
    vec3 R = reflect(-V, s.normal);
    vec3 env_r = eval_tod_sky(R, false);
    vec3 Fr = fresnel_schlick(s.clearcoat > 0.5 ? vec3(0.047) : F0, ndv);
    ambient += env_r * Fr * mix(1.0, 0.35, s.roughness) * s.occlusion;
  }

  return direct + ambient + s.emissive;
}
