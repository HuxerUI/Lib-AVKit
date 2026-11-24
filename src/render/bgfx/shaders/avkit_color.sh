vec4 normalizeColor(vec4 color, float alphaMode, float transfer) {
    if (transfer > 1.5) {
        // Invert the display conversion for a BT.709 encoder surface.
        vec3 low = pow(color.rgb / 12.92, vec3(1.0 / 2.4));
        color.rgb = mix(low, (color.rgb + 0.055) / 1.055, step(vec3(0.04045), color.rgb));
    } else if (transfer > 0.5) {
        // BT.1886 display linearization followed by sRGB encoding. The upper branch cancels the exponents.
        vec3 linearRgb = pow(color.rgb, vec3(2.4));
        color.rgb = mix(12.92 * linearRgb, 1.055 * color.rgb - 0.055, step(vec3(0.0031308), linearRgb));
    }
    if (alphaMode > 1.5) {
        color.rgb *= color.a;
    } else if (alphaMode > 0.5) {
        color.a = 1.0;
    }
    return color;
}
