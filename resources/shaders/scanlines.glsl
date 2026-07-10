// Example custom post-process fragment shader for post_process_shader_path.
//
// This file is concatenated after a fixed preamble that declares:
//   xe_source  - sampler2D of the guest output image
//   xe_uv      - vec2 UV coordinate for the current fragment
//   xe_frag_color - vec4 output color (location 0)
//   SourceSize - vec4(source width, source height, 1/width, 1/height) of the
//                guest's actual rendered resolution (see crt.slang)
//
// Only a `void main()` (and any helper declarations) should be defined here.

void main() {
  vec4 color = texture(xe_source, xe_uv);

  // One scanline per rendered source pixel row - fixed to the guest's actual
  // rendered resolution (SourceSize), not the window/output size.
  float scanline = sin(xe_uv.y * SourceSize.y * 3.14159265) * 0.5 + 0.5;
  float intensity = mix(0.75, 1.0, scanline);

  xe_frag_color = vec4(color.rgb * intensity, color.a);
}
