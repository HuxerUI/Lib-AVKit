vec4 normalizeAlpha(vec4 color, float mode) {
    if (mode > 1.5) {
        color.rgb *= color.a;
    } else if (mode > 0.5) {
        color.a = 1.0;
    }
    return color;
}
