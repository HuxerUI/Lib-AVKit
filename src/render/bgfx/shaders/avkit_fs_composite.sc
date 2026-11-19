$input v_texcoord0

#include <bgfx_shader.sh>
#include "avkit_alpha.sh"

uniform vec4 u_input_alpha;

SAMPLER2D(s_source, 0);

void main() {
    gl_FragColor = normalizeAlpha(texture2D(s_source, v_texcoord0), u_input_alpha.x);
}
