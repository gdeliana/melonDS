/*
 * Adapted for melonDS from libretro/glsl-shaders:
 * xbrz/shaders/xbrz-freescale.glsl
 * https://github.com/libretro/glsl-shaders
 * Uses source-resolution edge classification and a lightweight display pass,
 * with texture arrays, clamped integer texel fetches, and per-screen scale
 * supplied by the screen layout (including rotation and HiDPI).
 *
 * Hyllian's xBR-vertex code and texel mapping
 * Copyright (C) 2011/2016 Hyllian - sergiogdb@gmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// This shader also uses code and/or concepts from xBRZ as it appears
// in the Desmume source code. The license for which is as follows:
//
// ****************************************************************************
// * This file is part of the HqMAME project. It is distributed under         *
// * GNU General Public License: http://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT gmx DOT de) - All Rights Reserved          *
// *                                                                          *
// * Additionally and as a special exception, the author gives permission     *
// * to link the code of this program with the MAME library (or with modified *
// * versions of MAME that use the same license as MAME), and distribute      *
// * linked combinations including the two. You must obey the GNU General     *
// * Public License in all respects for all of the code used other than MAME. *
// * If you modify this file, you may extend this exception to your version   *
// * of the file, but you are not obligated to do so. If you do not wish to   *
// * do so, delete this exception statement from your version.                *
// ****************************************************************************

#ifndef XBRZ_SHADER_H
#define XBRZ_SHADER_H

// Classify edges once per source texel, not once per window pixel.
const char* kScreenVS_XbrzAnalysis = R"(#version 140
uniform int uLayer;
smooth out vec3 fTexcoord;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    fTexcoord = vec3(p, float(uLayer));
}
)";

const char* kScreenFS_XbrzAnalysis = R"(#version 140

uniform sampler2DArray ScreenTex;
smooth in vec3 fTexcoord;
out uint oInfo;

#define BLEND_NONE 0
#define BLEND_NORMAL 1
#define BLEND_DOMINANT 2
#define EQUAL_COLOR_TOLERANCE (30.0 / 255.0)
#define STEEP_DIRECTION_THRESHOLD 2.2
#define DOMINANT_DIRECTION_THRESHOLD 3.6

float DistYCbCrSquared(vec3 pixA, vec3 pixB)
{
    const vec3 w = vec3(0.2627, 0.6780, 0.0593);
    const float scaleB = 0.5 / (1.0 - w.b);
    const float scaleR = 0.5 / (1.0 - w.r);
    vec3 diff = pixA - pixB;
    float Y = dot(diff, w);
    float Cb = scaleB * (diff.b - Y);
    float Cr = scaleR * (diff.r - Y);
    return Y * Y + Cb * Cb + Cr * Cr;
}

float DistYCbCr(vec3 pixA, vec3 pixB)
{
    // Corner classification sums distances, so these square roots are needed.
    return sqrt(DistYCbCrSquared(pixA, pixB));
}

bool IsPixEqual(vec3 pixA, vec3 pixB)
{
    return DistYCbCrSquared(pixA, pixB) < EQUAL_COLOR_TOLERANCE * EQUAL_COLOR_TOLERANCE;
}

// Five bits per corner: active, line blend, shallow, steep, second neighbor.
uint packCorner(bool line, bool shallow, bool steep, bool second)
{
    return 1u | (line ? 2u : 0u) | (shallow ? 4u : 0u) |
           (steep ? 8u : 0u) | (second ? 16u : 0u);
}

vec3 samplePixel(ivec2 coord)
{
    ivec2 size = textureSize(ScreenTex, 0).xy;
    return texelFetch(ScreenTex, ivec3(clamp(coord, ivec2(0), size - 1), int(fTexcoord.z)), 0).rgb;
}

#define eq(a,b) (a == b)
#define neq(a,b) (a != b)
#define P(x,y) samplePixel(coord + ivec2(x,y))

void main()
{
    vec2 texel = fTexcoord.xy * vec2(textureSize(ScreenTex, 0).xy);
    ivec2 coord = ivec2(floor(texel));

    // Input pixel mapping: -|x|x|x|-
    //                       x|A|B|C|x
    //                       x|D|E|F|x
    //                       x|G|H|I|x
    //                       -|x|x|x|-
    vec3 A = P(-1,-1);
    vec3 B = P( 0,-1);
    vec3 C = P( 1,-1);
    vec3 D = P(-1, 0);
    vec3 E = P( 0, 0);
    vec3 F = P( 1, 0);
    vec3 G = P(-1, 1);
    vec3 H = P( 0, 1);
    vec3 I = P( 1, 1);

    // All four corners are inactive when the center and its axial neighbors
    // match. Avoid the outer texture taps and distance tests in flat regions.
    oInfo = 0u;
    if (eq(E,B) && eq(E,D) && eq(E,F) && eq(E,H)) return;

    // Corner mapping: x|y
    //                 w|z
    ivec4 blendResult = ivec4(BLEND_NONE);

    if (!((eq(E,F) && eq(H,I)) || (eq(E,H) && eq(F,I))))
    {
        float dist_H_F = DistYCbCr(G,E) + DistYCbCr(E,C) + DistYCbCr(P(0,2),I) + DistYCbCr(I,P(2,0)) + 4.0 * DistYCbCr(H,F);
        float dist_E_I = DistYCbCr(D,H) + DistYCbCr(H,P(1,2)) + DistYCbCr(B,F) + DistYCbCr(F,P(2,1)) + 4.0 * DistYCbCr(E,I);
        bool dominantGradient = DOMINANT_DIRECTION_THRESHOLD * dist_H_F < dist_E_I;
        blendResult.z = (dist_H_F < dist_E_I && neq(E,F) && neq(E,H)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }
    if (!((eq(D,E) && eq(G,H)) || (eq(D,G) && eq(E,H))))
    {
        float dist_G_E = DistYCbCr(P(-2,1),D) + DistYCbCr(D,B) + DistYCbCr(P(-1,2),H) + DistYCbCr(H,F) + 4.0 * DistYCbCr(G,E);
        float dist_D_H = DistYCbCr(P(-2,0),G) + DistYCbCr(G,P(0,2)) + DistYCbCr(A,E) + DistYCbCr(E,I) + 4.0 * DistYCbCr(D,H);
        bool dominantGradient = DOMINANT_DIRECTION_THRESHOLD * dist_D_H < dist_G_E;
        blendResult.w = (dist_G_E > dist_D_H && neq(E,D) && neq(E,H)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }
    if (!((eq(B,C) && eq(E,F)) || (eq(B,E) && eq(C,F))))
    {
        float dist_E_C = DistYCbCr(D,B) + DistYCbCr(B,P(1,-2)) + DistYCbCr(H,F) + DistYCbCr(F,P(2,-1)) + 4.0 * DistYCbCr(E,C);
        float dist_B_F = DistYCbCr(A,E) + DistYCbCr(E,I) + DistYCbCr(P(0,-2),C) + DistYCbCr(C,P(2,0)) + 4.0 * DistYCbCr(B,F);
        bool dominantGradient = DOMINANT_DIRECTION_THRESHOLD * dist_B_F < dist_E_C;
        blendResult.y = (dist_E_C > dist_B_F && neq(E,B) && neq(E,F)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }
    if (!((eq(A,B) && eq(D,E)) || (eq(A,D) && eq(B,E))))
    {
        float dist_D_B = DistYCbCr(P(-2,0),A) + DistYCbCr(A,P(0,-2)) + DistYCbCr(G,E) + DistYCbCr(E,C) + 4.0 * DistYCbCr(D,B);
        float dist_A_E = DistYCbCr(P(-2,-1),D) + DistYCbCr(D,H) + DistYCbCr(P(-1,-2),B) + DistYCbCr(B,F) + 4.0 * DistYCbCr(A,E);
        bool dominantGradient = DOMINANT_DIRECTION_THRESHOLD * dist_D_B < dist_A_E;
        blendResult.x = (dist_D_B < dist_A_E && neq(E,D) && neq(E,B)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
    }

    if (blendResult.z != BLEND_NONE)
    {
        float dist_F_G = DistYCbCr(F,G);
        float dist_H_C = DistYCbCr(H,C);
        bool doLineBlend = (blendResult.z == BLEND_DOMINANT ||
            !((blendResult.y != BLEND_NONE && !IsPixEqual(E,G)) || (blendResult.w != BLEND_NONE && !IsPixEqual(E,C)) ||
              (IsPixEqual(G,H) && IsPixEqual(H,I) && IsPixEqual(I,F) && IsPixEqual(F,C) && !IsPixEqual(E,I))));
        bool shallow = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_F_G <= dist_H_C) && neq(E,G) && neq(D,G);
        bool steep = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_H_C <= dist_F_G) && neq(E,C) && neq(B,C);
        oInfo |= packCorner(doLineBlend, shallow, steep, DistYCbCrSquared(E,F) <= DistYCbCrSquared(E,H)) << 10u;
    }
    if (blendResult.w != BLEND_NONE)
    {
        float dist_H_A = DistYCbCr(H,A);
        float dist_D_I = DistYCbCr(D,I);
        bool doLineBlend = (blendResult.w == BLEND_DOMINANT ||
            !((blendResult.z != BLEND_NONE && !IsPixEqual(E,A)) || (blendResult.x != BLEND_NONE && !IsPixEqual(E,I)) ||
              (IsPixEqual(A,D) && IsPixEqual(D,G) && IsPixEqual(G,H) && IsPixEqual(H,I) && !IsPixEqual(E,G))));
        bool shallow = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_H_A <= dist_D_I) && neq(E,A) && neq(B,A);
        bool steep = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_D_I <= dist_H_A) && neq(E,I) && neq(F,I);
        oInfo |= packCorner(doLineBlend, shallow, steep, DistYCbCrSquared(E,D) <= DistYCbCrSquared(E,H)) << 15u;
    }
    if (blendResult.y != BLEND_NONE)
    {
        float dist_B_I = DistYCbCr(B,I);
        float dist_F_A = DistYCbCr(F,A);
        bool doLineBlend = (blendResult.y == BLEND_DOMINANT ||
            !((blendResult.x != BLEND_NONE && !IsPixEqual(E,I)) || (blendResult.z != BLEND_NONE && !IsPixEqual(E,A)) ||
              (IsPixEqual(I,F) && IsPixEqual(F,C) && IsPixEqual(C,B) && IsPixEqual(B,A) && !IsPixEqual(E,C))));
        bool shallow = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_B_I <= dist_F_A) && neq(E,I) && neq(H,I);
        bool steep = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_F_A <= dist_B_I) && neq(E,A) && neq(D,A);
        oInfo |= packCorner(doLineBlend, shallow, steep, DistYCbCrSquared(E,B) <= DistYCbCrSquared(E,F)) << 5u;
    }
    if (blendResult.x != BLEND_NONE)
    {
        float dist_D_C = DistYCbCr(D,C);
        float dist_B_G = DistYCbCr(B,G);
        bool doLineBlend = (blendResult.x == BLEND_DOMINANT ||
            !((blendResult.w != BLEND_NONE && !IsPixEqual(E,C)) || (blendResult.y != BLEND_NONE && !IsPixEqual(E,G)) ||
              (IsPixEqual(C,B) && IsPixEqual(B,A) && IsPixEqual(A,D) && IsPixEqual(D,G) && !IsPixEqual(E,A))));
        bool shallow = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_D_C <= dist_B_G) && neq(E,C) && neq(F,C);
        bool steep = doLineBlend && (STEEP_DIRECTION_THRESHOLD * dist_B_G <= dist_D_C) && neq(E,G) && neq(H,G);
        oInfo |= packCorner(doLineBlend, shallow, steep, DistYCbCrSquared(E,B) <= DistYCbCrSquared(E,D));
    }
}
)";

const char* kScreenFS_Xbrz = R"(#version 140
uniform sampler2DArray ScreenTex;
uniform usampler2DArray XbrzInfoTex;
// Lengths of the four scaled edge normals divided by their squared lengths.
// Computed once per screen draw, rather than per corner of every output pixel.
uniform vec4 uNormalScale;
smooth in vec3 fTexcoord;
out vec4 oColor;

vec3 samplePixel(ivec2 coord)
{
    ivec2 size = textureSize(ScreenTex, 0).xy;
    return texelFetch(ScreenTex, ivec3(clamp(coord, ivec2(0), size - 1), int(fTexcoord.z)), 0).rgb;
}

void blendCorner(inout vec3 res, int code, vec3 first, vec3 second,
                 vec2 pos, vec4 normalScale)
{
    if (code == 0) return;
    bool line = (code & 2) != 0;
    bool shallow = (code & 4) != 0;
    bool steep = (code & 8) != 0;
    float origin = line ? (shallow ? 0.25 : 0.5) : 1.0 / sqrt(2.0);
    vec2 normal = vec2(steep ? 2.0 : 1.0, shallow ? 2.0 : 1.0);
    int shape = (shallow ? 1 : 0) + (steep ? 2 : 0);
    float distance = dot(pos - vec2(0.0, origin), normal) * normalScale[shape];
    float ratio = smoothstep(-sqrt(2.0) / 2.0, sqrt(2.0) / 2.0, distance);
    res = mix(res, (code & 16) != 0 ? second : first, ratio);
}

void main()
{
    vec2 texel = fTexcoord.xy * vec2(textureSize(ScreenTex, 0).xy);
    ivec2 coord = ivec2(floor(texel));
    vec3 res = samplePixel(coord);
    uint info = texelFetch(XbrzInfoTex, ivec3(coord, int(fTexcoord.z)), 0).r;
    if (info != 0u)
    {
        vec2 pos = fract(texel) - vec2(0.5);
        vec3 B = samplePixel(coord + ivec2(0,-1));
        vec3 D = samplePixel(coord + ivec2(-1,0));
        vec3 F = samplePixel(coord + ivec2(1,0));
        vec3 H = samplePixel(coord + ivec2(0,1));
        blendCorner(res, int((info >> 10u) & 31u), H, F, pos, uNormalScale);
        blendCorner(res, int((info >> 15u) & 31u), H, D, vec2(pos.y, -pos.x), uNormalScale.xzyw);
        blendCorner(res, int((info >> 5u) & 31u), F, B, vec2(-pos.y, pos.x), uNormalScale.xzyw);
        blendCorner(res, int(info & 31u), D, B, -pos, uNormalScale);
    }
    oColor = vec4(res, 1.0);
}
)";

#endif // XBRZ_SHADER_H
