/* Ground appearance, evaluated per pixel in world space.
 * Desktop is GLSL 330. The web build is OpenGL ES 2.0 (GLSL 100).
 * Both share the same body. Vertex colours carry the cover weights
 * (city, farm, forest, highland), not the final albedo. */
#ifndef BALLOON_GROUND_SHADER_H
#define BALLOON_GROUND_SHADER_H

static const char GROUND_VS_330[] =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec3 vertexNormal;\n"
    "in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform float uWater;\n"
    "out vec3 vWorld;\n"
    "out vec3 vNormal;\n"
    "out vec4 vCover;\n"
    "void main() {\n"
    "    vec3 p = vertexPosition;\n"
    "    if (p.y < uWater) p.y = uWater;\n"
    "    vWorld = p;\n"
    "    vNormal = vertexNormal;\n"
    "    vCover = vertexColor;\n"
    "    gl_Position = mvp * vec4(p, 1.0);\n"
    "}\n";

static const char GROUND_VS_100[] =
    "#version 100\n"
    "precision highp float;\n"
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "attribute vec3 vertexNormal;\n"
    "attribute vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform float uWater;\n"
    "varying vec3 vWorld;\n"
    "varying vec3 vNormal;\n"
    "varying vec4 vCover;\n"
    "void main() {\n"
    "    vec3 p = vertexPosition;\n"
    "    if (p.y < uWater) p.y = uWater;\n"
    "    vWorld = p;\n"
    "    vNormal = vertexNormal;\n"
    "    vCover = vertexColor;\n"
    "    gl_Position = mvp * vec4(p, 1.0);\n"
    "}\n";

#define GROUND_FS_BODY \
    "uniform vec4 colDiffuse;\n" \
    "uniform vec2 uEye;\n" \
    "uniform float uWater;\n" \
    "const float FIELD = 128.0;\n" \
    "const float BLOCK = 64.0;\n" \
    "float sat(float x) { return clamp(x, 0.0, 1.0); }\n" \
    "float hash12(vec2 p) {\n" \
    "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n" \
    "    p3 += dot(p3, p3.yzx + 33.33);\n" \
    "    return fract((p3.x + p3.y) * p3.z);\n" \
    "}\n" \
    "vec3 cropOf(float h) {\n" \
    "    if (h < 0.2) return vec3(206.0, 176.0, 64.0) / 255.0;\n" \
    "    if (h < 0.4) return vec3(78.0, 154.0, 64.0) / 255.0;\n" \
    "    if (h < 0.6) return vec3(158.0, 106.0, 64.0) / 255.0;\n" \
    "    if (h < 0.8) return vec3(138.0, 174.0, 78.0) / 255.0;\n" \
    "    return vec3(220.0, 196.0, 62.0) / 255.0;\n" \
    "}\n" \
    "vec3 blockOf(float h) {\n" \
    "    if (h < 0.34) return vec3(168.0, 164.0, 156.0) / 255.0;\n" \
    "    if (h < 0.67) return vec3(186.0, 178.0, 166.0) / 255.0;\n" \
    "    return vec3(150.0, 156.0, 162.0) / 255.0;\n" \
    "}\n" \
    "float fogAmount(float dist) {\n" \
    "    float d = max(dist, 0.0);\n" \
    "    float h = 1.0 - exp(-d / 5200.0);\n" \
    "    float far = sat((d - 1500.0) / 14000.0);\n" \
    "    return sat(h * 0.82 + far * far * 0.18);\n" \
    "}\n" \
    "vec3 applyHaze(vec3 c, float fog) {\n" \
    "    vec3 haze = vec3(150.0, 188.0, 214.0) / 255.0;\n" \
    "    vec3 horizon = vec3(232.0, 210.0, 180.0) / 255.0;\n" \
    "    vec3 washed = mix(c, haze, sat(fog));\n" \
    "    float intoSky = sat((fog - 0.75) / 0.25);\n" \
    "    return mix(washed, horizon, intoSky);\n" \
    "}\n" \
    "float shoreAlpha;\n" \
    "vec3 landColor(vec3 world, vec3 nrm, vec4 cover) {\n" \
    "    shoreAlpha = 1.0;\n" \
    "    float above = world.y - uWater;\n" \
    "    float aa = max(fwidth(world.y), 0.12);\n" \
    "    float dry = smoothstep(0.0, aa, above);\n" \
    "    float city = sat(cover.r);\n" \
    "    float farm = sat(cover.g);\n" \
    "    float forest = sat(cover.b);\n" \
    "    float highland = sat(cover.a);\n" \
    "    float plains = sat(1.0 - city - farm - forest - highland);\n" \
    "    float patch = hash12(floor(world.xz * 0.07));\n" \
    "    vec3 grassA = vec3(86.0, 166.0, 74.0) / 255.0;\n" \
    "    vec3 grassB = vec3(58.0, 132.0, 62.0) / 255.0;\n" \
    "    vec3 forestA = vec3(34.0, 92.0, 48.0) / 255.0;\n" \
    "    vec3 forestB = vec3(24.0, 70.0, 40.0) / 255.0;\n" \
    "    vec3 grass = mix(grassA, grassB, patch);\n" \
    "    vec3 canopy = mix(forestA, forestB, patch);\n" \
    "    vec3 highC = mix(grassB, forestA, 0.35);\n" \
    "    vec2 fuv = world.xz / FIELD;\n" \
    "    vec2 fu = fract(fuv);\n" \
    "    float hedge = max(1.0 - smoothstep(0.045, 0.08, fu.x), 1.0 - smoothstep(0.045, 0.08, fu.y));\n" \
    "    vec3 field = mix(cropOf(hash12(floor(fuv))), vec3(58.0, 104.0, 52.0) / 255.0, hedge);\n" \
    "    field = mix(grass, field, smoothstep(uWater + 0.4, uWater + 1.4, world.y));\n" \
    "    vec2 buv = world.xz / BLOCK;\n" \
    "    vec2 bu = fract(buv);\n" \
    "    float street = max(1.0 - smoothstep(0.14, 0.20, bu.x), 1.0 - smoothstep(0.14, 0.20, bu.y));\n" \
    "    float bh = hash12(floor(buv));\n" \
    "    vec3 cityC = mix(blockOf(hash12(floor(buv) + vec2(3.0, -1.0))), mix(grassA, grassB, 0.4), 1.0 - step(1.0 / 11.0, bh));\n" \
    "    cityC = mix(cityC, vec3(74.0, 78.0, 84.0) / 255.0, street);\n" \
    "    cityC = mix(grass, cityC, smoothstep(uWater + 0.2, uWater + 1.2, world.y));\n" \
    "    float sum = max(plains + farm + forest + highland + city, 0.0001);\n" \
    "    vec3 c = (grass * plains + field * farm + canopy * forest + highC * highland + cityC * city) / sum;\n" \
    "    float bank = smoothstep(-aa, aa, above);\n" \
    "    float shore = (1.0 - sat(farm + city)) * bank * (1.0 - smoothstep(3.2, 4.8, max(above, 0.0)));\n" \
    "    vec3 sand = vec3(214.0, 196.0, 138.0) / 255.0;\n" \
    "    c = mix(c, mix(sand, c, smoothstep(0.0, 4.0, max(above, 0.0))), shore);\n" \
    "    float foam = (1.0 - sat(farm + city)) * bank * (1.0 - smoothstep(0.0, 1.4, max(above, 0.0)));\n" \
    "    c = mix(c, vec3(232.0, 236.0, 226.0) / 255.0, foam * 0.7);\n" \
    "    float rockT = sat((0.88 - nrm.y) / 0.30) * highland + sat((0.70 - nrm.y) / 0.28) * (1.0 - sat(farm + city)) * (1.0 - highland);\n" \
    "    c = mix(c, vec3(142.0, 138.0, 128.0) / 255.0, sat(rockT));\n" \
    "    c = mix(c, vec3(236.0, 242.0, 246.0) / 255.0, sat((world.y - 52.0) / 18.0));\n" \
    "    vec3 water = vec3(64.0, 150.0, 196.0) / 255.0;\n" \
    "    c = mix(water, c, dry);\n" \
    "    return applyHaze(c, fogAmount(length(world.xz - uEye)));\n" \
    "}\n"

static const char GROUND_FS_330[] =
    "#version 330\n"
    "in vec3 vWorld;\n"
    "in vec3 vNormal;\n"
    "in vec4 vCover;\n"
    "out vec4 finalColor;\n"
    GROUND_FS_BODY
    "void main() {\n"
    "    vec3 nrm = normalize(vNormal);\n"
    "    vec3 c = landColor(vWorld, nrm, vCover);\n"
    "    finalColor = vec4(c, shoreAlpha) * colDiffuse;\n"
    "}\n";

static const char GROUND_FS_100[] =
    "#version 100\n"
    "#extension GL_OES_standard_derivatives : enable\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec3 vWorld;\n"
    "varying vec3 vNormal;\n"
    "varying vec4 vCover;\n"
    GROUND_FS_BODY
    "void main() {\n"
    "    vec3 nrm = normalize(vNormal);\n"
    "    vec3 c = landColor(vWorld, nrm, vCover);\n"
    "    gl_FragColor = vec4(c, shoreAlpha) * colDiffuse;\n"
    "}\n";

#endif
