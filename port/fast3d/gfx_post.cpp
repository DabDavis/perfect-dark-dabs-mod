/**
 * SMAA and FSR 1's shaders for both renderers. See gfx_post.h.
 */

#include <stdint.h>
#include <string.h>
#include <string>

#include "gfx_post.h"
#include "post/gfx_post_src.h"
#include "post/AreaTex.h"
#include "post/SearchTex.h"

static const char *gfx_post_names[GFX_POST_NUM_PASSES] = {
    "SMAA edges", "SMAA weights", "SMAA blend", "FSR EASU", "FSR RCAS", "copy",
};

const char *gfx_post_pass_name(GfxPostPass pass) {
    return (unsigned)pass < GFX_POST_NUM_PASSES ? gfx_post_names[pass] : "?";
}

std::string gfx_post_vertex_shader(const GfxPostLang &lang) {
    std::string s = std::string("#version ") + lang.version + "\n";
    if (lang.vulkan) {
        s += "layout(location = 0) out vec2 vUV;\n"
             "#define VERTEX_ID gl_VertexIndex\n";
    } else {
        s += "out vec2 vUV;\n"
             "#define VERTEX_ID gl_VertexID\n";
    }
    s += "void main() {\n"
         "    vec2 p = vec2(float((VERTEX_ID << 1) & 2), float(VERTEX_ID & 2));\n"
         "    vUV = p;\n"
         "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
         "}\n";
    return s;
}

// The declarations every pass shares: TEX0-2 as something texture() takes,
// and SMAA_ARG0-2 as what SMAA's functions take for a texture
static std::string gfx_post_prelude(const GfxPostLang &lang) {
    std::string s = std::string("#version ") + lang.version + "\n";
    if (lang.vulkan) {
        s += "layout(set = 0, binding = 0) uniform texture2D uTextures[" + std::to_string(lang.texture_slots) + "];\n";
        s += "layout(set = 0, binding = 1) uniform sampler uSamplers[" + std::to_string(lang.sampler_slots) + "];\n";
        s += "layout(push_constant) uniform Push { int t0; int t1; int t2; int smp; vec4 uParams; };\n"
             "layout(location = 0) in vec2 vUV;\n"
             "layout(location = 0) out vec4 oCol;\n"
             "#define POST_TEX(t) sampler2D(uTextures[t], uSamplers[smp])\n"
             "#define TEX0 POST_TEX(t0)\n"
             "#define TEX1 POST_TEX(t1)\n"
             "#define TEX2 POST_TEX(t2)\n"
             "#define SMAA_ARG0 t0\n"
             "#define SMAA_ARG1 t1\n"
             "#define SMAA_ARG2 t2\n";
    } else {
        s += "uniform sampler2D uTex0;\n"
             "uniform sampler2D uTex1;\n"
             "uniform sampler2D uTex2;\n"
             "uniform vec4 uParams;\n"
             "in vec2 vUV;\n"
             "out vec4 oCol;\n"
             "#define TEX0 uTex0\n"
             "#define TEX1 uTex1\n"
             "#define TEX2 uTex2\n"
             "#define SMAA_ARG0 uTex0\n"
             "#define SMAA_ARG1 uTex1\n"
             "#define SMAA_ARG2 uTex2\n";
    }
    return s;
}

static std::string gfx_post_smaa(const GfxPostLang &lang) {
    std::string s;
    if (lang.vulkan) {
        // A texture is its slot in the bindless table: Vulkan's GLSL cannot
        // hand a sampler2D built from a texture and a sampler to a function
        s += "#define SMAA_CUSTOM_SL 1\n"
             "#define SMAATexture2D(tex) int tex\n"
             "#define SMAATexturePass2D(tex) tex\n"
             "#define SMAASampleLevelZero(tex, coord) textureLod(POST_TEX(tex), coord, 0.0)\n"
             "#define SMAASampleLevelZeroPoint(tex, coord) textureLod(POST_TEX(tex), coord, 0.0)\n"
             "#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(POST_TEX(tex), coord, 0.0, offset)\n"
             "#define SMAASample(tex, coord) texture(POST_TEX(tex), coord)\n"
             "#define SMAASamplePoint(tex, coord) texture(POST_TEX(tex), coord)\n"
             "#define SMAASampleOffset(tex, coord, offset) textureOffset(POST_TEX(tex), coord, offset)\n"
             "#define SMAA_FLATTEN\n"
             "#define SMAA_BRANCH\n"
             "#define lerp(a, b, t) mix(a, b, t)\n"
             "#define saturate(a) clamp(a, 0.0, 1.0)\n"
             "#define mad(a, b, c) (a * b + c)\n"
             "#define float2 vec2\n"
             "#define float3 vec3\n"
             "#define float4 vec4\n"
             "#define int2 ivec2\n"
             "#define int3 ivec3\n"
             "#define int4 ivec4\n"
             "#define bool2 bvec2\n"
             "#define bool3 bvec3\n"
             "#define bool4 bvec4\n";
    } else {
        s += "#define SMAA_GLSL_3 1\n";
    }
    // Every pass runs at the size of what it reads, so the metrics come from
    // TEX0; the vertex halves of the passes run here in the fragment shader
    s += "vec4 gMetrics;\n"
         "#define SMAA_RT_METRICS gMetrics\n"
         "#define SMAA_PRESET_HIGH 1\n"
         "#define SMAA_INCLUDE_VS 1\n"
         "#define SMAA_INCLUDE_PS 1\n";
    s += gfx_post_smaa_src;
    s += "\nvoid setMetrics() {\n"
         "    vec2 size = vec2(textureSize(TEX0, 0));\n"
         "    gMetrics = vec4(1.0 / size, size);\n"
         "}\n";
    return s;
}

static std::string gfx_post_fsr(const char *which) {
    std::string s = "#define A_GPU 1\n"
                    "#define A_GLSL 1\n";
    s += gfx_post_ffx_a_src;
    s += std::string("\n#define ") + which + " 1\n";
    s += gfx_post_ffx_fsr1_src;
    s += "\n";
    return s;
}

std::string gfx_post_fragment_shader(const GfxPostLang &lang, GfxPostPass pass) {
    std::string s = gfx_post_prelude(lang);

    switch (pass) {
        case GFX_POST_SMAA_EDGES:
            s += gfx_post_smaa(lang);
            s += "void main() {\n"
                 "    setMetrics();\n"
                 "    vec4 offset[3];\n"
                 "    SMAAEdgeDetectionVS(vUV, offset);\n"
                 "    oCol = vec4(SMAALumaEdgeDetectionPS(vUV, offset, SMAA_ARG0), 0.0, 0.0);\n"
                 "}\n";
            break;
        case GFX_POST_SMAA_WEIGHTS:
            s += gfx_post_smaa(lang);
            s += "void main() {\n"
                 "    setMetrics();\n"
                 "    vec2 pixcoord;\n"
                 "    vec4 offset[3];\n"
                 "    SMAABlendingWeightCalculationVS(vUV, pixcoord, offset);\n"
                 "    oCol = SMAABlendingWeightCalculationPS(vUV, pixcoord, offset, SMAA_ARG0, SMAA_ARG1, SMAA_ARG2,\n"
                 "                                           vec4(0.0));\n"
                 "}\n";
            break;
        case GFX_POST_SMAA_BLEND:
            s += gfx_post_smaa(lang);
            s += "void main() {\n"
                 "    setMetrics();\n"
                 "    vec4 offset;\n"
                 "    SMAANeighborhoodBlendingVS(vUV, offset);\n"
                 "    oCol = vec4(SMAANeighborhoodBlendingPS(vUV, offset, SMAA_ARG0, SMAA_ARG1).rgb, 1.0);\n"
                 "}\n";
            break;
        case GFX_POST_EASU:
            s += gfx_post_fsr("FSR_EASU_F");
            s += "AF4 FsrEasuRF(AF2 p) { return textureGather(TEX0, p, 0); }\n"
                 "AF4 FsrEasuGF(AF2 p) { return textureGather(TEX0, p, 1); }\n"
                 "AF4 FsrEasuBF(AF2 p) { return textureGather(TEX0, p, 2); }\n"
                 "void main() {\n"
                 "    vec2 size = vec2(textureSize(TEX0, 0));\n"
                 "    AU4 con0, con1, con2, con3;\n"
                 "    FsrEasuCon(con0, con1, con2, con3, size.x, size.y, size.x, size.y, uParams.x, uParams.y);\n"
                 "    AF3 c;\n"
                 "    FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);\n"
                 "    oCol = vec4(c, 1.0);\n"
                 "}\n";
            break;
        case GFX_POST_RCAS:
            s += gfx_post_fsr("FSR_RCAS_F");
            s += "AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(TEX0, clamp(p, ASU2(0), textureSize(TEX0, 0) - 1), 0); }\n"
                 "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
                 "void main() {\n"
                 "    AU4 con;\n"
                 "    FsrRcasCon(con, uParams.x);\n"
                 "    AF3 c;\n"
                 "    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), con);\n"
                 "    oCol = vec4(c, 1.0);\n"
                 "}\n";
            break;
        default:
            s += "void main() {\n"
                 "    oCol = vec4(texture(TEX0, vUV).rgb, 1.0);\n"
                 "}\n";
            break;
    }

    return s;
}

const uint8_t *gfx_post_area_rgba(void) {
    static uint8_t *rgba;
    if (!rgba) {
        rgba = new uint8_t[AREATEX_WIDTH * AREATEX_HEIGHT * 4];
        for (int i = 0; i < AREATEX_WIDTH * AREATEX_HEIGHT; i++) {
            rgba[i * 4 + 0] = areaTexBytes[i * 2 + 0];
            rgba[i * 4 + 1] = areaTexBytes[i * 2 + 1];
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 255;
        }
    }
    return rgba;
}

const uint8_t *gfx_post_search_rgba(void) {
    static uint8_t *rgba;
    if (!rgba) {
        rgba = new uint8_t[SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT * 4];
        for (int i = 0; i < SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT; i++) {
            rgba[i * 4 + 0] = searchTexBytes[i];
            rgba[i * 4 + 1] = 0;
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 255;
        }
    }
    return rgba;
}
