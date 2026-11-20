$input v_texcoord0

#include <bgfx_shader.sh>
#include "avkit_alpha.sh"

uniform vec4 u_input_alpha;
uniform vec4 u_input_flips;
uniform vec4 u_sampling[6];

SAMPLER2D(s_source, 0);
SAMPLER2D(s_mask, 2);

vec2 mapUv(vec2 uv, vec4 x, vec4 y) {
    return vec2(dot(uv, x.xy) + x.z, dot(uv, y.xy) + y.z);
}

void main() {
    vec2 uv = mapUv(v_texcoord0, u_sampling[0], u_sampling[1]);
    uv.y = mix(uv.y, 1.0 - uv.y, u_input_flips.x);
    vec4 foreground = normalizeAlpha(texture2D(s_source, uv), u_input_alpha.x);
    if (u_sampling[0].w > 0.5) {
        vec2 maskUv = mapUv(v_texcoord0, u_sampling[2], u_sampling[3]);
        vec4 valid = u_sampling[4];
        float coverage = 0.0;
        vec2 halfTexel = vec2(u_sampling[2].w, u_sampling[3].w);
        vec2 texel = 2.0 * halfTexel;
        vec2 first = ceil((valid.xy - halfTexel) / texel);
        vec2 end = ceil((valid.xy + valid.zw - halfTexel) / texel);
        if (all(greaterThanEqual(maskUv, valid.xy)) && all(lessThan(maskUv, valid.xy + valid.zw)) &&
            all(greaterThan(end, first))) {
            maskUv = clamp(maskUv, first * texel + halfTexel, end * texel - halfTexel);
            maskUv.y = mix(maskUv.y, 1.0 - maskUv.y, u_input_flips.z);
            coverage = texture2D(s_mask, maskUv).r;
            coverage = mix(coverage, 1.0 - coverage, u_sampling[1].w);
        }
        foreground *= coverage;
        foreground += u_sampling[5] * (1.0 - foreground.a);
    }
    gl_FragColor = foreground;
}
