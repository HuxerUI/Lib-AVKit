$input v_texcoord0, v_targetcoord

#include <bgfx_shader.sh>
#include "avkit_color.sh"

uniform vec4 u_input_color;

SAMPLER2D(s_source, 0);
SAMPLER2D(s_incoming, 1);
SAMPLER2D(s_mask, 2);

uniform vec4 u_input_flips;
uniform vec4 u_mask_parameters;
uniform vec4 u_mask_region;

vec2 sourceUv(vec2 uv, float flipV) {
    return vec2(uv.x, mix(uv.y, 1.0 - uv.y, flipV));
}

void main() {
    vec4 outgoing = normalizeColor(texture2D(s_source, sourceUv(v_texcoord0, u_input_flips.x)), u_input_color.x, u_input_color.z);
    vec4 incoming = normalizeColor(texture2D(s_incoming, sourceUv(v_texcoord0, u_input_flips.y)), u_input_color.y, u_input_color.w);
    vec2 maskUv = clamp((v_texcoord0 - u_mask_region.xy) / u_mask_region.zw, vec2(0.0), vec2(1.0));
    float coverage = texture2D(s_mask, sourceUv(maskUv, u_input_flips.z)).r;
    coverage = mix(coverage, 1.0 - coverage, u_mask_parameters.y);
    float feather = u_mask_parameters.x * 0.5;
    float weight = feather > 0.0 ? smoothstep(0.5 - feather, 0.5 + feather, coverage) : step(0.5, coverage);
    gl_FragColor = mix(outgoing, incoming, weight) * u_input_flips.w;
}
