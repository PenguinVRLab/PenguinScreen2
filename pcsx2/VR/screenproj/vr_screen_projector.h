#ifndef VR_SCREEN_PROJECTOR_H
#define VR_SCREEN_PROJECTOR_H

#include <math.h>
#include <stddef.h>
#include <string.h>

#define VRSP_PI 3.14159265358979323846
#define VRSP_MAX_CYLINDERS 8
#define VRSP_OUTLINE_SAMPLES 720

#define VRSP_EYE_LEFT  1u
#define VRSP_EYE_RIGHT 2u
#define VRSP_EYE_BOTH  3u

enum {
    VRSP_OK = 0,
    VRSP_ERR_RADIUS = 1,
    VRSP_ERR_ANGLE = 2,
    VRSP_ERR_ASPECT = 3,
    VRSP_ERR_MASK = 4,
    VRSP_ERR_FULL = 5,
    VRSP_ERR_ARG = 6
};

typedef struct { float x, y, z; } vrsp_vec3;
typedef struct { float x, y, z, w; } vrsp_quat;
typedef struct { vrsp_quat orientation; vrsp_vec3 position; } vrsp_pose;
typedef struct { float angle_left, angle_right, angle_up, angle_down; } vrsp_fov;
typedef struct { int x, y, w, h; } vrsp_rect;

typedef struct {
    vrsp_pose pose;
    float radius;
    float central_angle;
    float aspect_ratio;
    unsigned eye_mask;
    int source;
} vrsp_cylinder;

typedef struct {
    vrsp_cylinder cyl[VRSP_MAX_CYLINDERS];
    int count;
} vrsp_cylinder_list;

typedef struct {
    float rot0[4];
    float rot1[4];
    float rot2[4];
    float origin[4];
    float params[4];
    int meta[4];
} vrsp_cylinder_uniform;

typedef struct {
    float tangents[4];
    float rect[4];
    float depth[4];
    int meta[4];
    vrsp_cylinder_uniform cyl[VRSP_MAX_CYLINDERS];
} vrsp_eye_uniforms;

typedef char vrsp_static_assert_cyl_uniform[(sizeof(vrsp_cylinder_uniform) == 96) ? 1 : -1];
typedef char vrsp_static_assert_eye_uniforms[(sizeof(vrsp_eye_uniforms) == 64 + VRSP_MAX_CYLINDERS * 96) ? 1 : -1];

typedef struct {
    int hit;
    float u, v;
    float depth;
    int source;
    int cylinder;
} vrsp_hit;

typedef struct {
    vrsp_pose pose;
    float width, height;
    int x0, x1;
    int source;
} vrsp_facet;

typedef struct {
    double facet_deg, depth_mm, wobble_px, texture_px;
    int layers_mono, layers_stereo;
} vrsp_facet_cost;

typedef struct { double x, y, z; } vrsp_dvec3;
typedef struct { double x, y, z, w; } vrsp_dquat;

static vrsp_dvec3 vrsp_d3(float x, float y, float z) { vrsp_dvec3 r; r.x = x; r.y = y; r.z = z; return r; }
static vrsp_dquat vrsp_dq(const vrsp_quat *q) { vrsp_dquat r; r.x = q->x; r.y = q->y; r.z = q->z; r.w = q->w; return r; }
static vrsp_dvec3 vrsp_dsub(vrsp_dvec3 a, vrsp_dvec3 b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
static vrsp_dvec3 vrsp_dadd(vrsp_dvec3 a, vrsp_dvec3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
static vrsp_dvec3 vrsp_dscale(vrsp_dvec3 a, double s) { a.x *= s; a.y *= s; a.z *= s; return a; }
static vrsp_dvec3 vrsp_dcross(vrsp_dvec3 a, vrsp_dvec3 b) {
    vrsp_dvec3 r; r.x = a.y * b.z - a.z * b.y; r.y = a.z * b.x - a.x * b.z; r.z = a.x * b.y - a.y * b.x; return r;
}
static vrsp_dquat vrsp_dconj(vrsp_dquat q) { q.x = -q.x; q.y = -q.y; q.z = -q.z; return q; }
static vrsp_dquat vrsp_dqnorm(vrsp_dquat q) {
    double n = sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 0) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
    return q;
}
static vrsp_dquat vrsp_dqmul(vrsp_dquat a, vrsp_dquat b) {
    vrsp_dquat r;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return r;
}
static vrsp_dvec3 vrsp_drotate(vrsp_dquat q, vrsp_dvec3 v) {
    vrsp_dvec3 qv = vrsp_d3(0, 0, 0), t;
    qv.x = q.x; qv.y = q.y; qv.z = q.z;
    t = vrsp_dscale(vrsp_dcross(qv, v), 2.0);
    return vrsp_dadd(vrsp_dadd(v, vrsp_dscale(t, q.w)), vrsp_dcross(qv, t));
}
static vrsp_dquat vrsp_dq_axis_y(double angle) {
    vrsp_dquat q; q.x = 0; q.y = sin(angle / 2); q.z = 0; q.w = cos(angle / 2); return q;
}
static vrsp_quat vrsp_fq(vrsp_dquat q) { vrsp_quat r; r.x = (float)q.x; r.y = (float)q.y; r.z = (float)q.z; r.w = (float)q.w; return r; }
static vrsp_vec3 vrsp_f3(vrsp_dvec3 v) { vrsp_vec3 r; r.x = (float)v.x; r.y = (float)v.y; r.z = (float)v.z; return r; }
static vrsp_dvec3 vrsp_pose_local(const vrsp_pose *p, vrsp_dvec3 w) {
    return vrsp_drotate(vrsp_dconj(vrsp_dq(&p->orientation)), vrsp_dsub(w, vrsp_d3(p->position.x, p->position.y, p->position.z)));
}
static vrsp_dvec3 vrsp_pose_world(const vrsp_pose *p, vrsp_dvec3 c) {
    return vrsp_dadd(vrsp_d3(p->position.x, p->position.y, p->position.z), vrsp_drotate(vrsp_dq(&p->orientation), c));
}

static int vrsp_cylinder_validate(const vrsp_cylinder *c) {
    if (!c) return VRSP_ERR_ARG;
    if (!(isfinite(c->radius) && c->radius > 0.0f)) return VRSP_ERR_RADIUS;
    if (!(isfinite(c->central_angle) && c->central_angle > 0.0f && c->central_angle < (float)(2.0 * VRSP_PI))) return VRSP_ERR_ANGLE;
    if (!(isfinite(c->aspect_ratio) && c->aspect_ratio > 0.0f)) return VRSP_ERR_ASPECT;
    if (c->eye_mask < 1u || c->eye_mask > 3u) return VRSP_ERR_MASK;
    return VRSP_OK;
}

static float vrsp_cylinder_height(const vrsp_cylinder *c) {
    return c->radius * c->central_angle / c->aspect_ratio;
}

static void vrsp_list_clear(vrsp_cylinder_list *l) { memset(l, 0, sizeof(*l)); }

static int vrsp_list_add(vrsp_cylinder_list *l, const vrsp_cylinder *c) {
    int err = vrsp_cylinder_validate(c);
    if (err) return err;
    if (l->count >= VRSP_MAX_CYLINDERS) return VRSP_ERR_FULL;
    l->cyl[l->count++] = *c;
    return VRSP_OK;
}

static int vrsp_visible_to(const vrsp_cylinder *c, int eye_index) {
    return (c->eye_mask & (eye_index == 0 ? VRSP_EYE_LEFT : VRSP_EYE_RIGHT)) != 0u;
}

static void vrsp_fov_tangents(const vrsp_fov *f, float out[4]) {
    out[0] = tanf(f->angle_left); out[1] = tanf(f->angle_right); out[2] = tanf(f->angle_up); out[3] = tanf(f->angle_down);
}

static int vrsp_pack_eye(const vrsp_cylinder_list *list, int eye_index, const vrsp_pose *eye,
                         const vrsp_fov *declared, vrsp_rect rect, vrsp_eye_uniforms *out) {
    int k, n = 0;
    if (!list || !eye || !declared || !out || (eye_index != 0 && eye_index != 1)) return VRSP_ERR_ARG;
    memset(out, 0, sizeof(*out));
    vrsp_fov_tangents(declared, out->tangents);
    out->rect[0] = (float)rect.x; out->rect[1] = (float)rect.y; out->rect[2] = (float)rect.w; out->rect[3] = (float)rect.h;
    out->depth[0] = 0.1f; out->depth[1] = 100.0f; out->depth[2] = 0.0f; out->depth[3] = 1.0f;
    out->meta[0] = eye_index;
    for (k = 0; k < list->count; ++k) {
        const vrsp_cylinder *c = &list->cyl[k];
        vrsp_cylinder_uniform *u;
        vrsp_dquat q_inv, q_eye;
        vrsp_dvec3 o, ex, ey, ez;
        int a;
        if (!vrsp_visible_to(c, eye_index)) continue;
        u = &out->cyl[n++];
        q_inv = vrsp_dconj(vrsp_dq(&c->pose.orientation));
        q_eye = vrsp_dq(&eye->orientation);
        ex = vrsp_drotate(q_inv, vrsp_drotate(q_eye, vrsp_d3(1, 0, 0)));
        ey = vrsp_drotate(q_inv, vrsp_drotate(q_eye, vrsp_d3(0, 1, 0)));
        ez = vrsp_drotate(q_inv, vrsp_drotate(q_eye, vrsp_d3(0, 0, 1)));
        u->rot0[0] = (float)ex.x; u->rot0[1] = (float)ey.x; u->rot0[2] = (float)ez.x; u->rot0[3] = 0.0f;
        u->rot1[0] = (float)ex.y; u->rot1[1] = (float)ey.y; u->rot1[2] = (float)ez.y; u->rot1[3] = 0.0f;
        u->rot2[0] = (float)ex.z; u->rot2[1] = (float)ey.z; u->rot2[2] = (float)ez.z; u->rot2[3] = 0.0f;
        o = vrsp_pose_local(&c->pose, vrsp_d3(eye->position.x, eye->position.y, eye->position.z));
        u->origin[0] = (float)o.x; u->origin[1] = (float)o.y; u->origin[2] = (float)o.z; u->origin[3] = c->radius;
        u->params[0] = c->central_angle;
        u->params[1] = vrsp_cylinder_height(c);
        u->params[2] = 1.0f / c->central_angle;
        u->params[3] = 1.0f / u->params[1];
        u->meta[0] = c->source; u->meta[1] = k;
        for (a = 2; a < 4; ++a) u->meta[a] = 0;
    }
    out->meta[1] = n;
    return VRSP_OK;
}

static vrsp_hit vrsp_cast_pixel(const vrsp_eye_uniforms *e, int i, int j) {
    vrsp_hit best;
    float t[3];
    int k;
    best.hit = 0; best.u = best.v = 0.0f; best.depth = 0.0f; best.source = -1; best.cylinder = -1;
    t[0] = e->tangents[0] + ((float)i + 0.5f) / e->rect[2] * (e->tangents[1] - e->tangents[0]);
    t[1] = e->tangents[2] - ((float)j + 0.5f) / e->rect[3] * (e->tangents[2] - e->tangents[3]);
    t[2] = -1.0f;
    for (k = 0; k < e->meta[1]; ++k) {
        const vrsp_cylinder_uniform *c = &e->cyl[k];
        float dx = c->rot0[0] * t[0] + c->rot0[1] * t[1] + c->rot0[2] * t[2];
        float dy = c->rot1[0] * t[0] + c->rot1[1] * t[1] + c->rot1[2] * t[2];
        float dz = c->rot2[0] * t[0] + c->rot2[1] * t[1] + c->rot2[2] * t[2];
        float ox = c->origin[0], oy = c->origin[1], oz = c->origin[2], r = c->origin[3];
        float A = dx * dx + dz * dz, B, C, h, s, px, py, pz, phi, u, v;
        if (A <= 0.0f) continue;
        B = ox * dx + oz * dz;
        C = ox * ox + oz * oz - r * r;
        h = B * B - A * C;
        if (h < 0.0f) continue;
        s = (-B + sqrtf(h)) / A;
        if (s <= 0.0f) continue;
        px = ox + s * dx; py = oy + s * dy; pz = oz + s * dz;
#ifdef VRSP_PLANTED_FAULT_XFLIP
        px = -px;
#endif
        phi = atan2f(px, -pz);
        u = 0.5f + phi * c->params[2];
        v = 0.5f - py * c->params[3];
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
        if (!best.hit || s < best.depth) {
            best.hit = 1; best.u = u; best.v = v; best.depth = s;
            best.source = c->meta[0]; best.cylinder = c->meta[1];
        }
    }
    return best;
}

#define VRSP_NEAR_M 1e-6

static int vrsp_direction_hits(const vrsp_cylinder_list *list, int eye_index, const vrsp_pose *eye, vrsp_dvec3 d_eye) {
    vrsp_dvec3 d_w = vrsp_drotate(vrsp_dq(&eye->orientation), d_eye);
    vrsp_dvec3 ep = vrsp_d3(eye->position.x, eye->position.y, eye->position.z);
    int k;
    for (k = 0; k < list->count; ++k) {
        const vrsp_cylinder *c = &list->cyl[k];
        vrsp_dvec3 o, d;
        double r = c->radius, H, A, B, C, h, s, hx, hy, hz, u, v;
        if (!vrsp_visible_to(c, eye_index)) continue;
        o = vrsp_pose_local(&c->pose, ep);
        d = vrsp_drotate(vrsp_dconj(vrsp_dq(&c->pose.orientation)), d_w);
        A = d.x * d.x + d.z * d.z;
        if (A <= 0.0) continue;
        B = o.x * d.x + o.z * d.z;
        C = o.x * o.x + o.z * o.z - r * r;
        h = B * B - A * C;
        if (h < 0.0) continue;
        s = (-B + sqrt(h)) / A;
        if (s <= 0.0) continue;
        hx = o.x + s * d.x; hy = o.y + s * d.y; hz = o.z + s * d.z;
        H = (double)c->radius * c->central_angle / c->aspect_ratio;
        u = 0.5 + atan2(hx, -hz) / c->central_angle;
        v = 0.5 - hy / H;
        if (u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0) return 1;
    }
    return 0;
}

static int vrsp_facing(double r, double ex, double ez, double phi) {
    return r - (ex * sin(phi) - ez * cos(phi)) >= -1e-12 * r;
}

typedef struct {
    const vrsp_pose *cyl_pose;
    vrsp_dquat q_inv;
    vrsp_dvec3 ep;
    double planes[5][4];
    double lo_x, hi_x, lo_y, hi_y;
    int cross;
} vrsp_box_acc;

static void vrsp_box_segment(vrsp_box_acc *acc, vrsp_dvec3 la, vrsp_dvec3 lb) {
    vrsp_dvec3 a = vrsp_drotate(acc->q_inv, vrsp_dsub(vrsp_pose_world(acc->cyl_pose, la), acc->ep));
    vrsp_dvec3 b = vrsp_drotate(acc->q_inv, vrsp_dsub(vrsp_pose_world(acc->cyl_pose, lb), acc->ep));
    double t0 = 0.0, t1 = 1.0;
    int i;
    if ((a.z < -1e-9) != (b.z < -1e-9)) acc->cross = 1;
    for (i = 0; i < 5; ++i) {
        const double *pl = acc->planes[i];
        double fa = pl[0] * a.x + pl[1] * a.y + pl[2] * a.z + pl[3];
        double fb = pl[0] * b.x + pl[1] * b.y + pl[2] * b.z + pl[3];
        if (fa < 0.0 && fb < 0.0) return;
        if (fa < 0.0) { double t = fa / (fa - fb); if (t > t0) t0 = t; }
        else if (fb < 0.0) { double t = fa / (fa - fb); if (t < t1) t1 = t; }
        if (t0 > t1) return;
    }
    for (i = 0; i < 2; ++i) {
        vrsp_dvec3 p = vrsp_dadd(a, vrsp_dscale(vrsp_dsub(b, a), i == 0 ? t0 : t1));
        double x = -p.x / p.z, y = -p.y / p.z;
        if (x < acc->lo_x) acc->lo_x = x;
        if (x > acc->hi_x) acc->hi_x = x;
        if (y < acc->lo_y) acc->lo_y = y;
        if (y > acc->hi_y) acc->hi_y = y;
    }
}

static vrsp_dvec3 vrsp_cyl_point(double r, double phi, double y) {
    vrsp_dvec3 p; p.x = r * sin(phi); p.y = y; p.z = -r * cos(phi); return p;
}

static int vrsp_tight_fov(const vrsp_cylinder_list *list, int eye_index, const vrsp_pose *eye,
                          const vrsp_fov *located, int width, int height, double margin,
                          vrsp_fov *declared, vrsp_rect *rect, int *crosses) {
    const double eps = 1e-9;
    double tl = tan((double)located->angle_left), tr = tan((double)located->angle_right);
    double tu = tan((double)located->angle_up), td = tan((double)located->angle_down);
    double px = (tr - tl) / width, py = (tu - td) / height;
    double lo_x, hi_x, lo_y, hi_y;
    vrsp_box_acc acc;
    int k, n, x0, x1, y0, y1;
    if (crosses) *crosses = 0;
    memset(rect, 0, sizeof(*rect));
    memset(&acc, 0, sizeof(acc));
    acc.q_inv = vrsp_dconj(vrsp_dq(&eye->orientation));
    acc.ep = vrsp_d3(eye->position.x, eye->position.y, eye->position.z);
    acc.planes[0][0] = 1.0;  acc.planes[0][2] = tl;
    acc.planes[1][0] = -1.0; acc.planes[1][2] = -tr;
    acc.planes[2][1] = 1.0;  acc.planes[2][2] = td;
    acc.planes[3][1] = -1.0; acc.planes[3][2] = -tu;
    acc.planes[4][2] = -1.0; acc.planes[4][3] = -VRSP_NEAR_M;
    acc.lo_x = acc.lo_y = INFINITY;
    acc.hi_x = acc.hi_y = -INFINITY;
    for (k = 0; k < list->count; ++k) {
        const vrsp_cylinder *c = &list->cyl[k];
        double r = c->radius, arc = c->central_angle, H = (double)c->radius * c->central_angle / c->aspect_ratio;
        double ex, ez, rho;
        vrsp_dvec3 e_loc;
        int side;
        if (!vrsp_visible_to(c, eye_index)) continue;
        acc.cyl_pose = &c->pose;
        e_loc = vrsp_pose_local(&c->pose, acc.ep);
        ex = e_loc.x; ez = e_loc.z;
        for (side = 0; side < 2; ++side) {
            double y = side == 0 ? H / 2.0 : -H / 2.0;
            for (n = 0; n < VRSP_OUTLINE_SAMPLES; ++n) {
                double pa = ((double)n / VRSP_OUTLINE_SAMPLES - 0.5) * arc;
                double pb = ((double)(n + 1) / VRSP_OUTLINE_SAMPLES - 0.5) * arc;
                if (vrsp_facing(r, ex, ez, pa) || vrsp_facing(r, ex, ez, pb))
                    vrsp_box_segment(&acc, vrsp_cyl_point(r, pa, y), vrsp_cyl_point(r, pb, y));
            }
        }
        for (side = 0; side < 2; ++side) {
            double phi = side == 0 ? -arc / 2.0 : arc / 2.0;
            if (vrsp_facing(r, ex, ez, phi))
                vrsp_box_segment(&acc, vrsp_cyl_point(r, phi, H / 2.0), vrsp_cyl_point(r, phi, -H / 2.0));
        }
        rho = hypot(ex, ez);
        if (rho > r) {
            double alpha = atan2(ez, ex), s = asin(r / rho);
            for (side = 0; side < 2; ++side) {
                double phi = side == 0 ? alpha + s : alpha + VRSP_PI - s;
                phi = atan2(sin(phi), cos(phi));
                if (fabs(phi) <= arc / 2.0)
                    vrsp_box_segment(&acc, vrsp_cyl_point(r, phi, H / 2.0), vrsp_cyl_point(r, phi, -H / 2.0));
            }
        }
    }
    lo_x = acc.lo_x; hi_x = acc.hi_x; lo_y = acc.lo_y; hi_y = acc.hi_y;
#ifndef VRSP_PLANTED_FAULT_NOCORNERS
    for (n = 0; n < 4; ++n) {
        vrsp_dvec3 d;
        d.x = (n & 1) ? tr : tl; d.y = (n & 2) ? td : tu; d.z = -1.0;
        if (vrsp_direction_hits(list, eye_index, eye, d)) {
            if (d.x < lo_x) lo_x = d.x;
            if (d.x > hi_x) hi_x = d.x;
            if (d.y < lo_y) lo_y = d.y;
            if (d.y > hi_y) hi_y = d.y;
        }
    }
#else
    (void)vrsp_direction_hits;
#endif
    if (lo_x == INFINITY) return 0;
    if (crosses) *crosses = acc.cross;
    lo_x -= margin + eps; hi_x += margin + eps; lo_y -= margin + eps; hi_y += margin + eps;
    if (lo_x < tl) lo_x = tl;
    if (hi_x > tr) hi_x = tr;
    if (lo_y < td) lo_y = td;
    if (hi_y > tu) hi_y = tu;
    x0 = (int)floor((lo_x - tl) / px); if (x0 < 0) x0 = 0;
    x1 = (int)ceil((hi_x - tl) / px); if (x1 > width) x1 = width;
    y0 = (int)floor((tu - hi_y) / py); if (y0 < 0) y0 = 0;
    y1 = (int)ceil((tu - lo_y) / py); if (y1 > height) y1 = height;
    if (x1 <= x0 || y1 <= y0) { if (crosses) *crosses = 0; return 0; }
    rect->x = x0; rect->y = y0; rect->w = x1 - x0; rect->h = y1 - y0;
    declared->angle_left = (float)atan(tl + x0 * px);
    declared->angle_right = (float)atan(tl + x1 * px);
    declared->angle_up = (float)atan(tu - y0 * py);
    declared->angle_down = (float)atan(tu - y1 * py);
    return 1;
}

static int vrsp_facets_place(const vrsp_cylinder *c, int K, int image_width, int overlap_texels, vrsp_facet *out) {
    int i;
    double H;
    if (!c || !out || K < 1 || image_width < K) return VRSP_ERR_ARG;
    if (vrsp_cylinder_validate(c)) return vrsp_cylinder_validate(c);
    H = vrsp_cylinder_height(c);
    for (i = 0; i < K; ++i) {
        int x0 = (int)floor((double)i * image_width / K + 0.5) - overlap_texels;
        int x1 = (int)floor((double)(i + 1) * image_width / K + 0.5) + overlap_texels;
        double phi0, phi1, phi_i, theta;
        vrsp_dvec3 centre;
        if (x0 < 0) x0 = 0;
        if (x1 > image_width) x1 = image_width;
        phi0 = ((double)x0 / image_width - 0.5) * c->central_angle;
        phi1 = ((double)x1 / image_width - 0.5) * c->central_angle;
        phi_i = 0.5 * (phi0 + phi1);
        theta = 0.5 * (phi1 - phi0);
        centre.x = c->radius * cos(theta) * sin(phi_i);
        centre.y = 0.0;
        centre.z = -c->radius * cos(theta) * cos(phi_i);
        out[i].pose.position = vrsp_f3(vrsp_pose_world(&c->pose, centre));
        out[i].pose.orientation = vrsp_fq(vrsp_dqnorm(vrsp_dqmul(vrsp_dq(&c->pose.orientation), vrsp_dq_axis_y(-phi_i))));
        out[i].width = (float)(2.0 * c->radius * sin(theta));
        out[i].height = (float)H;
        out[i].x0 = x0; out[i].x1 = x1;
        out[i].source = c->source;
    }
    return VRSP_OK;
}

static vrsp_facet_cost vrsp_facet_cost_row(double arc, double radius, double aspect, double px_per_deg, int K) {
    vrsp_facet_cost r;
    double theta = arc / (2.0 * K), H = radius * arc / aspect, tan_t = tan(theta), deg = 180.0 / VRSP_PI;
    int n;
    r.facet_deg = 2.0 * theta * deg;
    r.depth_mm = radius * (1.0 - cos(theta)) * 1000.0;
    r.wobble_px = (atan((H / 2) / (radius * cos(theta))) - atan((H / 2) / radius)) * deg * px_per_deg;
    r.texture_px = 0.0;
    for (n = 0; n <= 2000; ++n) {
        double f = n / 2000.0, e = fabs(atan(f * tan_t) - f * theta) * deg * px_per_deg;
        if (e > r.texture_px) r.texture_px = e;
    }
    r.layers_mono = K; r.layers_stereo = 2 * K;
    return r;
}

#endif
