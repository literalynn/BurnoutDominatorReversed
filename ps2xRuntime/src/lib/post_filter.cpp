#include "runtime/post_filter.h"

namespace
{
    // FXAA in its console form: the luma gradient of the four diagonal neighbours gives the
    // edge direction, two or four taps along it blend the edge, and the wider blend is kept
    // only while its luma stays within the neighbourhood's range.
    const char *kFxaaShader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 texelSize;
out vec4 finalColor;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
    vec3 m = texture(texture0, fragTexCoord).rgb;
    vec3 nw = texture(texture0, fragTexCoord + vec2(-1.0, -1.0) * texelSize).rgb;
    vec3 ne = texture(texture0, fragTexCoord + vec2(1.0, -1.0) * texelSize).rgb;
    vec3 sw = texture(texture0, fragTexCoord + vec2(-1.0, 1.0) * texelSize).rgb;
    vec3 se = texture(texture0, fragTexCoord + vec2(1.0, 1.0) * texelSize).rgb;
    float lm = luma(m), lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se);
    float lowest = min(lm, min(min(lnw, lne), min(lsw, lse)));
    float highest = max(lm, max(max(lnw, lne), max(lsw, lse)));
    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2(-8.0), vec2(8.0)) * texelSize;
    vec3 a = 0.5 * (texture(texture0, fragTexCoord + dir * (1.0 / 3.0 - 0.5)).rgb +
                    texture(texture0, fragTexCoord + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(texture0, fragTexCoord - dir * 0.5).rgb +
                               texture(texture0, fragTexCoord + dir * 0.5).rgb);
    float lb = luma(b);
    finalColor = vec4((lb < lowest || lb > highest) ? a : b, 1.0);
}
)";

    Shader g_fxaa{};
    int g_texelSize = -1;
    RenderTexture2D g_target{};
}

namespace ps2x
{
    PostFrame applyPostFilter(const Texture2D &frame, bool fxaa, bool smooth)
    {
        if (!fxaa || frame.id == 0)
            return {frame, false};
        if (g_fxaa.id == 0)
        {
            g_fxaa = LoadShaderFromMemory(nullptr, kFxaaShader);
            g_texelSize = GetShaderLocation(g_fxaa, "texelSize");
        }
        if (g_target.id == 0 || g_target.texture.width != frame.width || g_target.texture.height != frame.height)
        {
            if (g_target.id != 0)
                UnloadRenderTexture(g_target);
            g_target = LoadRenderTexture(frame.width, frame.height);
        }
        SetTextureFilter(g_target.texture, smooth ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
        const float texel[2] = {1.0f / float(frame.width), 1.0f / float(frame.height)};
        SetShaderValue(g_fxaa, g_texelSize, texel, SHADER_UNIFORM_VEC2);
        BeginTextureMode(g_target);
        BeginShaderMode(g_fxaa);
        DrawTexture(frame, 0, 0, WHITE);
        EndShaderMode();
        EndTextureMode();
        return {g_target.texture, true};
    }
}
