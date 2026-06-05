//------------------------------------------------------------------------------
// Punctual lights evaluation
//------------------------------------------------------------------------------

// Make sure this matches the same constants in Froxel.cpp
#define FROXEL_BUFFER_WIDTH_SHIFT   6u
#define FROXEL_BUFFER_WIDTH         (1u << FROXEL_BUFFER_WIDTH_SHIFT)
#define FROXEL_BUFFER_WIDTH_MASK    (FROXEL_BUFFER_WIDTH - 1u)

#define LIGHT_TYPE_POINT            0u
#define LIGHT_TYPE_SPOT             1u
#define LIGHT_TYPE_RECT             2u


struct FroxelParams {
    uint recordOffset; // offset at which the list of lights for this froxel starts
    uint count;   // number lights in this froxel
};

/**
 * Returns the coordinates of the froxel at the specified fragment coordinates.
 * The coordinates are a 3D position in the froxel grid.
 */
uvec3 getFroxelCoords(const highp vec3 fragCoords) {
    uvec3 froxelCoord;

    froxelCoord.xy = uvec2(fragCoords.xy * frameUniforms.froxelCountXY);

    // go from screen-space to reciprocal of normalized view-space Z (i.e. scaled by 1/zLightFar)
    // we get away with the reciprocal because 1/z is handled by the log2() below.
    // see Froxelizer.cpp
    highp float viewSpaceNormalizedZ = frameUniforms.zParams.x * fragCoords.z + frameUniforms.zParams.y;

    // frameUniforms.zParams.w is actually the number of z-slices, make sure it's mediump
    float zSliceCount = frameUniforms.zParams.w;

    // compute the sliceZ mapping in highp, store in mediump
    float sliceZWithoutOffset = log2(viewSpaceNormalizedZ) * frameUniforms.zParams.z;

    // finally discretize the mapping into slices
    // We need to clamp because the far plane (z=1) is out of bounds, any smaller z is not.
    froxelCoord.z = uint(clamp(sliceZWithoutOffset + zSliceCount, 0.0, zSliceCount - 1.0));

    return froxelCoord;
}

/**
 * Computes the froxel index of the fragment at the specified coordinates.
 * The froxel index is computed from the 3D coordinates of the froxel in the
 * froxel grid and later used to fetch from the froxel buffer.
 */
uint getFroxelIndex(const highp vec3 fragCoords) {
    uvec3 froxelCoord = getFroxelCoords(fragCoords);
    return froxelCoord.x * frameUniforms.fParams.x +
           froxelCoord.y * frameUniforms.fParams.y +
           froxelCoord.z * frameUniforms.fParams.z;
}

/**
 * Computes the texture coordinates of the froxel data given a froxel index.
 */
ivec2 getFroxelTexCoord(uint froxelIndex) {
    return ivec2(froxelIndex & FROXEL_BUFFER_WIDTH_MASK, froxelIndex >> FROXEL_BUFFER_WIDTH_SHIFT);
}

/**
 * Returns the froxel data for the given froxel index. The data is fetched
 * from FroxelsUniforms UBO.
 */
FroxelParams getFroxelParams(const uint froxelIndex) {
    uint w = froxelIndex >> 2u;
    uint c = froxelIndex & 0x3u;
    highp uvec4 d = froxelsUniforms.records[w];
    highp uint f = d[c];
    FroxelParams froxel;
    froxel.recordOffset = f >> 16u;
    froxel.count = f & 0xFFu;
    return froxel;
}

/**
 * Return the light index from the record index
 * A light record is a single uint index into the lights data buffer (lightsUniforms UBO).
 */
uint getLightIndex(const uint index) {
    uint v = index >> 4u;
    uint c = (index >> 2u) & 0x3u;
    uint s = (index & 0x3u) * 8u;
    // this intermediate is needed to workaround a bug on qualcomm h/w
    highp uvec4 d = froxelRecordUniforms.records[v];
    return (d[c] >> s) & 0xFFu;
}

float getSquareFalloffAttenuation(float distanceSquare, float falloff) {
    float factor = distanceSquare * falloff;
    float smoothFactor = saturate(1.0 - factor * factor);
    // We would normally divide by the square distance here
    // but we do it at the call site
    return smoothFactor * smoothFactor;
}

float getDistanceAttenuation(const highp vec3 posToLight, float falloff) {
    float distanceSquare = dot(posToLight, posToLight);
    float attenuation = getSquareFalloffAttenuation(distanceSquare, falloff);
    // light far attenuation
    highp vec3 v = getWorldPosition() - getWorldCameraPosition();
    attenuation *= saturate(frameUniforms.lightFarAttenuationParams.x - dot(v, v) * frameUniforms.lightFarAttenuationParams.y);
    // Assume a punctual light occupies a volume of 1cm to avoid a division by 0
    return attenuation / max(distanceSquare, 1e-4);
}

float getAngleAttenuation(const highp vec3 lightDir, const highp vec3 l, const highp vec2 scaleOffset) {
    float cd = dot(lightDir, l);
    float attenuation = saturate(cd * scaleOffset.x + scaleOffset.y);
    return attenuation * attenuation;
}

/**
 * Returns a Light structure (see surface_lighting.fs) describing a point or spot light.
 * The colorIntensity field will store the *pre-exposed* intensity of the light
 * in the w component.
 *
 * The light parameters used to compute the Light structure are fetched from the
 * lightsUniforms uniform buffer.
 */

Light getLight(const uint lightIndex) {
    // retrieve the light data from the UBO

    highp mat4 data = lightsUniforms.lights[lightIndex];

    highp vec4 positionFalloff = data[0];
    highp vec3 direction = data[1].xyz;
    vec4 colorIES = vec4(
        unpackHalf2x16(floatBitsToUint(data[2][0])),
        unpackHalf2x16(floatBitsToUint(data[2][1]))
    );
    highp vec2 scaleOffset = data[2].zw;
    highp float intensity = data[3][1];
    highp uint typeShadow = floatBitsToUint(data[3][2]);
    highp uint channels = floatBitsToUint(data[3][3]);

    // poition-to-light vector
    highp vec3 worldPosition = getWorldPosition();
    highp vec3 posToLight = positionFalloff.xyz - worldPosition;

    // and populate the Light structure
    Light light;
    light.colorIntensity.rgb = colorIES.rgb;
    light.colorIntensity.w = computePreExposedIntensity(intensity, frameUniforms.exposure);
    light.l = normalize(posToLight);
    light.attenuation = getDistanceAttenuation(posToLight, positionFalloff.w);
    light.direction = direction;
    light.NoL = saturate(dot(shading_normal, light.l));
    light.worldPosition = positionFalloff.xyz;
    light.channels = int(channels);
    light.contactShadows = bool(typeShadow & 0x10u);
    light.rectEdge1 = vec3(0.0);
    light.rectEdge2 = vec3(0.0);
#if defined(VARIANT_HAS_DYNAMIC_LIGHTING)
    light.lightType = (typeShadow & 0xFu);
#if defined(VARIANT_HAS_SHADOWING)
    light.shadowIndex = int((typeShadow >>  8u) & 0xFFu);
    light.castsShadows   = bool(channels & 0x10000u);
    if (light.lightType == LIGHT_TYPE_SPOT) {
        light.zLight = dot(shadowUniforms.shadows[light.shadowIndex].lightFromWorldZ, vec4(worldPosition, 1.0));
    }
#endif
    if (light.lightType == LIGHT_TYPE_SPOT) {
        light.attenuation *= getAngleAttenuation(-direction, light.l, scaleOffset);
    } else if (light.lightType == LIGHT_TYPE_RECT) {
        light.rectEdge1 = vec3(data[1].w, scaleOffset.x, scaleOffset.y);
        vec2 e2yz = unpackHalf2x16(typeShadow >> 16u);
        light.rectEdge2 = vec3(data[3][0], e2yz.x, e2yz.y);
        light.attenuation = getDistanceAttenuation(posToLight, positionFalloff.w);
    }
#endif
    return light;
}

float rectEdgeIrradiance(vec3 N, vec3 P, vec3 v0, vec3 v1) {
    vec3 L0 = v0 - P;
    vec3 L1 = v1 - P;
    vec3 dir0 = normalize(L0);
    vec3 dir1 = normalize(L1);
    float cos0 = dot(N, dir0);
    float cos1 = dot(N, dir1);
    float sin0 = length(cross(N, dir0));
    float sin1 = length(cross(N, dir1));
    float comp = acos(clamp(dot(dir0, dir1), -1.0, 1.0));
    if (abs(sin0) < 1e-5 || abs(sin1) < 1e-5) {
        return 0.0;
    }
    return (comp - sin0 * cos0 - sin1 * cos1) * (0.5 / PI);
}

vec3 evaluateRectAreaLight(const PixelParams pixel, const Light light) {
    vec3 N = shading_normal;
    vec3 P = getWorldPosition();
    vec3 c = light.worldPosition;
    vec3 e1 = light.rectEdge1;
    vec3 e2 = light.rectEdge2;
    vec3 n = light.direction;

    vec3 toCenter = c - P;
    if (dot(n, toCenter) <= 0.0) {
        return vec3(0.0);
    }

    vec3 v0 = c - e1 - e2;
    vec3 v1 = c + e1 - e2;
    vec3 v2 = c + e1 + e2;
    vec3 v3 = c - e1 + e2;

    float irradiance = 0.0;
    irradiance += rectEdgeIrradiance(N, P, v0, v1);
    irradiance += rectEdgeIrradiance(N, P, v1, v2);
    irradiance += rectEdgeIrradiance(N, P, v2, v3);
    irradiance += rectEdgeIrradiance(N, P, v3, v0);

    return light.colorIntensity.rgb * light.colorIntensity.w * irradiance * pixel.diffuseColor;
}

/**
 * Evaluates all punctual lights that my affect the current fragment.
 * The result of the lighting computations is accumulated in the color
 * parameter, as linear HDR RGB.
 */
void evaluatePunctualLights(const MaterialInputs material,
        const PixelParams pixel, inout vec3 color) {

    // Fetch the light information stored in the froxel that contains the
    // current fragment
    FroxelParams froxel = getFroxelParams(getFroxelIndex(getNormalizedPhysicalViewportCoord()));

    // Each froxel contains how many lights can influence
    // the current fragment. A froxel also contains a record offset that
    // tells us where the indices of those lights are in the records
    // buffer. The records buffer contains the indices of the actual
    // light data in the lightsUniforms UBO.

    uint index = froxel.recordOffset;
    uint end = index + froxel.count;
    int channels = object_uniforms_flagsChannels & 0xFF;

    // Iterate point lights
    for ( ; index < end; index++) {
        uint lightIndex = getLightIndex(index);
        Light light = getLight(lightIndex);
        if ((light.channels & channels) == 0) {
            continue;
        }

#if defined(MATERIAL_CAN_SKIP_LIGHTING)
        if (light.lightType != LIGHT_TYPE_RECT &&
                (light.NoL <= 0.0 || light.attenuation <= 0.0)) {
            continue;
        }
#endif

        float visibility = 1.0;
#if defined(VARIANT_HAS_SHADOWING)
        if (light.NoL > 0.0) {
            if (light.castsShadows) {
                int shadowIndex = light.shadowIndex;
                if (light.lightType == LIGHT_TYPE_POINT) {
                    // point-light shadows are sampled from a direction
                    highp vec3 r = getWorldPosition() - light.worldPosition;
                    int face = getPointLightFace(r);
                    shadowIndex += face;
                    light.zLight = dot(shadowUniforms.shadows[shadowIndex].lightFromWorldZ,
                            vec4(getWorldPosition(), 1.0));
                }
                highp vec4 shadowPosition = getShadowPosition(shadowIndex, light.direction, light.zLight);
                visibility = shadow(false, sampler0_shadowMap, shadowIndex,
                        shadowPosition, light.zLight);
#if defined(MATERIAL_HAS_SHADOW_STRENGTH)
                applyShadowStrength(visibility, material.shadowStrength);
#endif
            }
            if (light.contactShadows && visibility > 0.0) {
                if ((object_uniforms_flagsChannels & FILAMENT_OBJECT_CONTACT_SHADOWS_BIT) != 0) {
                    visibility *= 1.0 - screenSpaceContactShadow(light.l);
                }
            }
#if defined(MATERIAL_CAN_SKIP_LIGHTING)
            if (visibility <= 0.0) {
                continue;
            }
#endif
        }
#endif

#if defined(MATERIAL_HAS_CUSTOM_SURFACE_SHADING)
        color.rgb += customSurfaceShading(material, pixel, light, visibility);
#else
        if (light.lightType == LIGHT_TYPE_RECT) {
            color.rgb += evaluateRectAreaLight(pixel, light) * visibility;
        } else {
            color.rgb += surfaceShading(pixel, light, visibility);
        }
#endif
    }

    if (CONFIG_DEBUG_FROXEL_VISUALIZATION) {
        if (froxel.count > 0u && frameUniforms.enableFroxelViz != 0) {
            const vec3 debugColors[17] = vec3[](
                vec3(0.0,     0.0,     0.0),         // black
                vec3(0.0,     0.0,     0.1647),      // darkest blue
                vec3(0.0,     0.0,     0.3647),      // darker blue
                vec3(0.0,     0.0,     0.6647),      // dark blue
                vec3(0.0,     0.0,     0.9647),      // blue
                vec3(0.0,     0.9255,  0.9255),      // cyan
                vec3(0.0,     0.5647,  0.0),         // dark green
                vec3(0.0,     0.7843,  0.0),         // green
                vec3(1.0,     1.0,     0.0),         // yellow
                vec3(0.90588, 0.75294, 0.0),         // yellow-orange
                vec3(1.0,     0.5647,  0.0),         // orange
                vec3(1.0,     0.0,     0.0),         // bright red
                vec3(0.8392,  0.0,     0.0),         // red
                vec3(1.0,     0.0,     1.0),         // magenta
                vec3(0.6,     0.3333,  0.7882),      // purple
                vec3(1.0,     1.0,     1.0),         // white
                vec3(1.0,     1.0,     1.0)          // white
            );
            color = mix(color, debugColors[clamp(froxel.count, 0u, 16u)], 0.8);
        }
    }
}
