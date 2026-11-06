#version 450

// example shader: soft vignette + faint scanlines. the frame comes in as `source`, the effects sliders
// as push constants. copy it, change main(), pick it in Settings > Effects

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D source;

layout(push_constant) uniform Effects {
  vec2 texel;
  float sharpen;
  float vibrance;
  float saturation;
  float contrast;
  float brightness;
  float gamma;
} fx;

void main() {
  vec3 c = texture(source, uv).rgb;
  float scanline = 0.94 + 0.06 * sin(uv.y / fx.texel.y * 3.14159265);
  vec2 offset = uv - 0.5;
  float vignette = 1.0 - dot(offset, offset) * 0.9;
  c = (c - 0.5) * fx.contrast + 0.5 + fx.brightness;
  color = vec4(clamp(c * scanline * vignette, 0.0, 1.0), 1.0);
}
