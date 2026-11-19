#version 450

#define VRSP_MAX_CYLINDERS 8
#define VRSP_MAX_SOURCES 4

struct VrspCylinder {
    vec4 rot0, rot1, rot2;
    vec4 origin;
    vec4 params;
    ivec4 meta;
};

layout(std140, set = 0, binding = 0) uniform VrspEye {
    vec4 tangents;
    vec4 rect;
    vec4 depth;
    ivec4 meta;
    VrspCylinder cyl[VRSP_MAX_CYLINDERS];
} eye;

layout(set = 0, binding = 1) uniform sampler2D sources[VRSP_MAX_SOURCES];

layout(location = 0) out vec4 out_color;

vec4 sample_source(int source, vec2 uv) {
    switch (source) {
        case 0: return texture(sources[0], uv);
        case 1: return texture(sources[1], uv);
        case 2: return texture(sources[2], uv);
        case 3: return texture(sources[3], uv);
        default: return vec4(1.0, 0.0, 1.0, 1.0);
    }
}

void main() {
    float i = gl_FragCoord.x - eye.rect.x;
    float j = gl_FragCoord.y - eye.rect.y;
    vec3 t = vec3(eye.tangents.x + i / eye.rect.z * (eye.tangents.y - eye.tangents.x),
                  eye.tangents.z - j / eye.rect.w * (eye.tangents.z - eye.tangents.w),
                  -1.0);

    float best_s = 0.0;
    int best = -1;
    vec2 best_uv = vec2(0.0);
    for (int k = 0; k < eye.meta.y; ++k) {
        vec3 d = vec3(dot(eye.cyl[k].rot0.xyz, t), dot(eye.cyl[k].rot1.xyz, t), dot(eye.cyl[k].rot2.xyz, t));
        vec3 o = eye.cyl[k].origin.xyz;
        float r = eye.cyl[k].origin.w;
        float A = d.x * d.x + d.z * d.z;
        if (A <= 0.0) continue;
        float B = o.x * d.x + o.z * d.z;
        float C = o.x * o.x + o.z * o.z - r * r;
        float h = B * B - A * C;
        if (h < 0.0) continue;
        float s = (-B + sqrt(h)) / A;
        if (s <= 0.0) continue;
        vec3 p = o + s * d;
        float phi = atan(p.x, -p.z);
        float u = 0.5 + phi * eye.cyl[k].params.z;
        float v = 0.5 - p.y * eye.cyl[k].params.w;
        if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) continue;
        if (best < 0 || s < best_s) { best_s = s; best = k; best_uv = vec2(u, v); }
    }

    if (best < 0) {
        out_color = vec4(0.0);
#ifdef VRSP_WRITE_DEPTH
        gl_FragDepth = eye.depth.w;
#endif
        return;
    }

    vec2 pitch = vec2((eye.tangents.y - eye.tangents.x) / eye.rect.z, (eye.tangents.z - eye.tangents.w) / eye.rect.w);
    float du_px = best_s * pitch.x * eye.cyl[best].params.z / eye.cyl[best].origin.w;
    float dv_px = best_s * pitch.y * eye.cyl[best].params.w;
    float edge_px = min(min(best_uv.x, 1.0 - best_uv.x) / max(du_px, 1e-7),
                        min(best_uv.y, 1.0 - best_uv.y) / max(dv_px, 1e-7));
    float coverage = clamp(edge_px + 0.5, 0.0, 1.0);

    vec4 c = sample_source(eye.cyl[best].meta.x, best_uv);
    out_color = vec4(c.rgb * coverage, coverage);

#ifdef VRSP_WRITE_DEPTH
    float nearZ = eye.depth.x, farZ = eye.depth.y;
    float z01 = (farZ * (best_s - nearZ)) / (best_s * (farZ - nearZ));
    gl_FragDepth = mix(eye.depth.z, eye.depth.w, clamp(z01, 0.0, 1.0));
#endif
}
