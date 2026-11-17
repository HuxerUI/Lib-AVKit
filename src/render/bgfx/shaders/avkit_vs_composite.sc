$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_destination;
uniform vec4 u_off_diagonal;

void main() {
    vec2 position = a_position.xy * u_destination.zw + a_position.yx * u_off_diagonal.xy;
    gl_Position = vec4(position + u_destination.xy, a_position.z, 1.0);
    v_texcoord0 = a_texcoord0;
}
