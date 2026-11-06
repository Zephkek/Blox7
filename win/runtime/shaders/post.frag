#version 450

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
  float fxaa;
} fx;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 smooth_edges(vec2 p) {
  vec2 t = fx.texel;
  vec3 m = texture(source, p).rgb;
  float nw = luma(texture(source, p + vec2(-1.0, -1.0) * t).rgb);
  float ne = luma(texture(source, p + vec2(1.0, -1.0) * t).rgb);
  float sw = luma(texture(source, p + vec2(-1.0, 1.0) * t).rgb);
  float se = luma(texture(source, p + vec2(1.0, 1.0) * t).rgb);
  float lm = luma(m);
  float lo = min(lm, min(min(nw, ne), min(sw, se)));
  float hi = max(lm, max(max(nw, ne), max(sw, se)));
  if (hi - lo < max(0.0312, hi * 0.125)) return m;
  vec2 dir = vec2((sw + se) - (nw + ne), (nw + sw) - (ne + se));
  float reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * t;
  vec3 near = 0.5 * (texture(source, p - dir / 6.0).rgb + texture(source, p + dir / 6.0).rgb);
  vec3 far = 0.5 * near + 0.25 * (texture(source, p - dir * 0.5).rgb + texture(source, p + dir * 0.5).rgb);
  float lf = luma(far);
  return lf < lo || lf > hi ? near : far;
}

void main() {
  vec3 c = fx.fxaa > 0.5 ? smooth_edges(uv) : texture(source, uv).rgb;
  if (fx.sharpen > 0.0) {
    vec3 n = texture(source, uv - vec2(0.0, fx.texel.y)).rgb;
    vec3 s = texture(source, uv + vec2(0.0, fx.texel.y)).rgb;
    vec3 w = texture(source, uv - vec2(fx.texel.x, 0.0)).rgb;
    vec3 e = texture(source, uv + vec2(fx.texel.x, 0.0)).rgb;
    vec3 lo = min(c, min(min(n, s), min(w, e)));
    vec3 hi = max(c, max(max(n, s), max(w, e)));
    // edges already near black or white get sharpened less
    vec3 amount = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0));
    vec3 weight = -amount / mix(8.0, 5.0, clamp(fx.sharpen, 0.0, 1.0));
    c = clamp((c + (n + s + w + e) * weight) / (1.0 + 4.0 * weight), 0.0, 1.0);
  }
  float spread = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
  c = mix(vec3(luma(c)), c, fx.saturation + fx.vibrance * (1.0 - spread));
  c = (c - 0.5) * fx.contrast + 0.5 + fx.brightness;
  c = pow(clamp(c, 0.0, 1.0), vec3(1.0 / max(fx.gamma, 0.1)));
  color = vec4(c, 1.0);
}
