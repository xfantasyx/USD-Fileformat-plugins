/*
Copyright 2023 Adobe. All rights reserved.
This file is licensed to you under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License. You may obtain a copy
of the License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed under
the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR REPRESENTATIONS
OF ANY KIND, either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/
#include <fileformatutils/layerWriteOpenPBR.h>

#include <fileformatutils/common.h>
#include <fileformatutils/debugCodes.h>
#include <fileformatutils/sdfMaterialUtils.h>
#include <fileformatutils/sdfUtils.h>

#include <pxr/usd/usdShade/tokens.h>

using namespace PXR_NS;

namespace adobe::usd {

const std::string stPrimvarNameAttrName = "stPrimvarName";

SdfPath
_createMaterialXUvReader(SdfAbstractData* sdfData, const SdfPath& parentPath, int uvIndex)
{
    // XXX The MaterialX texcoord reader function has an index to specify which set of UV
    // coordinates to read, but it does not have the ability to specify a primvar by name. So we
    // currently default to the first set, but there is something to be figured out about how to
    // connect a named primvar to a UV coordinate index in MaterialX.
    // Maybe ND_geompropvalue_vector2 with geomprop="st" will do the trick. Note, that the shared
    // stPrimvarNameAttrName input attribute is of type Token, but `geomprop` is of type String
    return createShader(sdfData,
                        parentPath,
                        getSTTexCoordReaderToken(uvIndex),
                        MtlXTokens->ND_texcoord_vector2,
                        "out");
}

// If a texture coordinate transform is needed for the given input a transform will be created and
// the result output path will be returned. Otherwise it will forward the default ST reader result
// path.
SdfPath
_createMaterialXUvTransform(SdfAbstractData* sdfData,
                            const SdfPath& parentPath,
                            const std::string& name,
                            const Input& input,
                            const SdfPath& uvReaderResultPath)
{
    if (input.hasDefaultTransform()) {
        return uvReaderResultPath;
    }

    // For the place2d node, the scale is not a multiplier, but the overall scale and so we need to
    // invert the value
    GfVec2f scale = input.uvScale;
    scale[0] = scale[0] != 0.0f ? 1.0f / scale[0] : 0.0f;
    scale[1] = scale[1] != 0.0f ? 1.0f / scale[1] : 0.0f;

    // Create UV transform by applying scale, rotation and transform, in that order
    // This matches what the UsdTransform2d node does
    return createShader(
      sdfData,
      parentPath,
      TfToken(name + "_uv_transform"),
      MtlXTokens->ND_place2d_vector2,
      "out",
      { { "scale", scale }, { "rotate", input.uvRotation }, { "offset", input.uvTranslation } },
      { { "texcoord", uvReaderResultPath } });
    ;
}

std::string
_toMaterialXAddressMode(const TfToken& wrapMode)
{
    if (wrapMode.IsEmpty()) {
        return "periodic";
    } else if (wrapMode == AdobeTokens->repeat) {
        return "periodic";
    } else if (wrapMode == AdobeTokens->clamp) {
        return "clamp";
    } else if (wrapMode == AdobeTokens->mirror) {
        return "mirror";
    } else if (wrapMode == AdobeTokens->black) {
        return "constant";
    } else {
        TF_WARN("Unknown wrapMode '%s'", wrapMode.GetText());
        return "periodic";
    }
}

SdfPath
_createScaleAndBiasNodes(SdfAbstractData* sdfData,
                         const SdfPath& parentPath,
                         const std::string& baseName,
                         const SdfPath& textureInput,
                         int numChannels,
                         bool isColor,
                         const GfVec4f& scale4,
                         const GfVec4f& bias4)
{
    TfToken scaleShaderType, biasShaderType;
    VtValue scale, bias;
    if (numChannels == 1) {
        float s = scale4[0];
        if (s != 1.0f) {
            scale = s;
            scaleShaderType = MtlXTokens->ND_multiply_float;
        }
        float b = bias4[0];
        if (b != 0.0f) {
            bias = b;
            biasShaderType = MtlXTokens->ND_add_float;
        }
    } else if (numChannels == 3) {
        GfVec3f s = GfVec3f(scale4[0], scale4[1], scale4[2]);
        if (s != GfVec3f(1.0f)) {
            scale = s;
            scaleShaderType =
              isColor ? MtlXTokens->ND_multiply_color3 : MtlXTokens->ND_multiply_vector3;
        }
        GfVec3f b = GfVec3f(bias4[0], bias4[1], bias4[2]);
        if (b != GfVec3f(0.0f)) {
            bias = b;
            biasShaderType = isColor ? MtlXTokens->ND_add_color3 : MtlXTokens->ND_add_vector3;
        }
    }

    SdfPath textureOutput = textureInput;
    if (!scale.IsEmpty()) {
        textureOutput = createShader(sdfData,
                                     parentPath,
                                     TfToken(baseName + "_scale"),
                                     scaleShaderType,
                                     "out",
                                     { { "in1", scale } },
                                     { { "in2", textureOutput } });
    }
    if (!bias.IsEmpty()) {
        textureOutput = createShader(sdfData,
                                     parentPath,
                                     TfToken(baseName + "_bias"),
                                     biasShaderType,
                                     "out",
                                     { { "in1", bias } },
                                     { { "in2", textureOutput } });
    }

    return textureOutput;
}

SdfPath
_createMaterialXTextureReader(SdfAbstractData* sdfData,
                              const SdfPath& parentPath,
                              const TfToken& name,
                              const Input& input,
                              const SdfPath& uvResultPath,
                              const SdfPath& textureConnection,
                              bool isNormalMap,
                              bool convertToColor)
{
    int numChannels = input.numChannels();
    TfToken shaderType;
    VtValue defaultValue;
    if (numChannels == 1) {
        // If we want to extract a single channel we read the RGBA version of the texture in linear
        // color space.
        shaderType = MtlXTokens->ND_image_vector4;
        if (input.value.IsHolding<float>()) {
            // We're always using a RGBA texture reader (ND_image_vector4), so the fallback value
            // has to match, even if we only care about a single channel.
            float f = input.value.UncheckedGet<float>();
            defaultValue = GfVec4f(f);
        }
    } else if (numChannels == 3) {
        // We differentiate between two types of texture readers depending on the type of input on
        // the surface shader. A mismatch in types will lead to errors.
        if (name == OpenPbrTokens->geometry_normal || name == OpenPbrTokens->geometry_coat_normal ||
            name == OpenPbrTokens->geometry_tangent) {
            shaderType = MtlXTokens->ND_image_vector3;
        } else {
            shaderType = MtlXTokens->ND_image_color3;
        }
        if (input.value.IsHolding<GfVec3f>()) {
            defaultValue = input.value;
        }
    } else {
        TF_CODING_ERROR(
          "Unsupported texture type for %d channels on input %s", numChannels, name.GetText());
        return SdfPath();
    }

    // In MaterialX, each input attribute on a node can have an associated color space. We
    // explicitly mark the "file" input with a color space if we know that we got a sRGB texture.
    // Note, this will become the "colorSpace" metadata on the input attribute.
    InputColorSpaces inputColorSpaces;
    if (input.colorspace == AdobeTokens->sRGB) {
        inputColorSpaces["file"] = MtlXTokens->srgb_texture;
    }

    InputValues inputValues = { { "default", defaultValue },
                                { "uaddressmode", _toMaterialXAddressMode(input.wrapS) },
                                { "vaddressmode", _toMaterialXAddressMode(input.wrapT) } };
    InputConnections inputConnections = { { "texcoord", uvResultPath },
                                          { "file", textureConnection } };

    // Note, we're setting the texture path directly on this texture reader, which means the
    // path is duplicated on each texture reader of the same texture for each of the different
    // sub networks. This is currently needed since some software is not correctly following
    // connections to resolve input values.
    // Once that has improved in the ecosystem we could author the asset path once as an
    // attribute on the material and connect all corresponding texture readers to that attribute
    // value.
    SdfPath textureOutput = createShader(sdfData,
                                         parentPath,
                                         name,
                                         shaderType,
                                         "out",
                                         inputValues,
                                         inputConnections,
                                         inputColorSpaces);

    // Extract the single channel from the 4 channel reader
    if (numChannels == 1) {
        std::string out = input.channel == AdobeTokens->r   ? "outx"
                          : input.channel == AdobeTokens->g ? "outy"
                          : input.channel == AdobeTokens->b ? "outz"
                                                            : "outw";
        textureOutput = createShader(sdfData,
                                     parentPath,
                                     TfToken(name.GetString() + "_to_float"),
                                     MtlXTokens->ND_separate4_vector4,
                                     out,
                                     {},
                                     { { "in", textureOutput } });
    }

    if (isNormalMap) {
        // The texture reader for a normal map reads a texture map in tangent space, which needs to
        // be transformed into world space. Route normal map through a normal map node
        // Note, we skip the usual scale and bias of 2 and -1 for the normal map data and send the
        // data directly into the normalmap node.
        textureOutput = createShader(sdfData,
                                     parentPath,
                                     TfToken(name.GetString() + "_to_world_space"),
                                     MtlXTokens->ND_normalmap,
                                     "out",
                                     {},
                                     { { "in", textureOutput } });
    } else {
        if (!input.hasDefaultScaleAndBias()) {
            bool isColor = shaderType == MtlXTokens->ND_image_color3;
            textureOutput = _createScaleAndBiasNodes(sdfData,
                                                     parentPath,
                                                     name.GetString(),
                                                     textureOutput,
                                                     numChannels,
                                                     isColor,
                                                     input.scale,
                                                     input.bias);
        }
    }

    if (convertToColor && numChannels == 1) {
        textureOutput = createShader(sdfData,
                                     parentPath,
                                     TfToken(name.GetString() + "_to_color"),
                                     MtlXTokens->ND_convert_float_color3,
                                     "out",
                                     {},
                                     { { "in", textureOutput } });
    }

    return textureOutput;
}

void
_setupOpenPbrInput(WriteSdfContext& ctx,
                   const SdfPath& materialPath,
                   const SdfPath& parentPath,
                   const TfToken& name,
                   const Input& input,
                   std::unordered_map<int, SdfPath>& uvReaderResultPathMap,
                   InputValues& inputValues,
                   InputConnections& inputConnections,
                   const InputToMaterialInputTypeMap& inputRemapping,
                   MaterialInputs& materialInputs)
{
    auto remappingIt = inputRemapping.find(name);
    bool hasMapping = remappingIt != inputRemapping.cend();
    if (!hasMapping) {
        TF_CODING_ERROR("Expecting to find remapping for shader input '%s'", name.GetText());
        return;
    }

    const TfToken& materialInputName = remappingIt->second.name;
    const SdfValueTypeName& inputType = remappingIt->second.type;

    if (input.image >= 0) {
        if (input.isZeroTexture()) {
            inputValues.emplace_back(name.GetString(), getTextureZeroVtValue(input.channel));
        } else {
            if ((size_t)input.image >= ctx.usdData->images.size()) {
                TF_CODING_ERROR("Image index %d for %s is larger than images array %zu",
                                input.image,
                                name.GetText(),
                                ctx.usdData->images.size());
                return;
            }
            std::string texturePath =
              createTexturePath(ctx.srcAssetFilename, ctx.usdData->images[input.image].uri);

            SdfPath textureConnection = addMaterialInputTexture(
              ctx.sdfData, materialPath, materialInputName, texturePath, materialInputs);

            // Create the ST reader on demand when we create the first textured input
            SdfPath uvReaderResultPath;
            auto it = uvReaderResultPathMap.find(input.uvIndex);
            if (it == uvReaderResultPathMap.end()) {
                uvReaderResultPath =
                  _createMaterialXUvReader(ctx.sdfData, parentPath, input.uvIndex);
                uvReaderResultPathMap[input.uvIndex] = uvReaderResultPath;
            } else {
                uvReaderResultPath = it->second;
            }

            // This creates a ST transform node if needed, otherwise the default ST result path
            // will be returned.
            SdfPath stResultPath = _createMaterialXUvTransform(
              ctx.sdfData, parentPath, name.GetString(), input, uvReaderResultPath);

            bool isNormalMap =
              name == OpenPbrTokens->geometry_normal || name == OpenPbrTokens->geometry_coat_normal;
            // geometry_opacity expects a color, but our input opacity is a float input
            bool convertToColor = name == OpenPbrTokens->geometry_opacity;
            SdfPath texResultPath = _createMaterialXTextureReader(ctx.sdfData,
                                                                  parentPath,
                                                                  name,
                                                                  input,
                                                                  stResultPath,
                                                                  textureConnection,
                                                                  isNormalMap,
                                                                  convertToColor);

            inputConnections.emplace_back(name.GetString(), texResultPath);
        }
    } else if (!input.value.IsEmpty()) {
        if (!materialInputName.IsEmpty()) {
            // Set constant value on material input and connect surface shader to that input
            SdfPath connection = addMaterialInputValue(
              ctx.sdfData, materialPath, materialInputName, inputType, input.value, materialInputs);
            inputConnections.emplace_back(name.GetString(), connection);
            const MinMaxVtValuePair* range =
              ShaderRegistry::getInstance().getMaterialInputRange(materialInputName);
            if (range) {
                setRangeMetadata(ctx.sdfData, connection, *range);
            }
        } else {
            // If the input name is not valid, then just set the value
            inputValues.emplace_back(name.GetString(), input.value);
        }
    }
}

void
writeOpenPBR(WriteSdfContext& ctx,
             const SdfPath& materialPath,
             const OpenPbrMaterial& material,
             MaterialInputs& materialInputs)
{
    SdfPath p;

    // This will create a NodeGraph parent prim for all the shading nodes in this network
    SdfPath parentPath =
      createPrimSpec(ctx.sdfData, materialPath, MtlXTokens->OpenPBR, UsdShadeTokens->NodeGraph);

    TF_DEBUG_MSG(FILE_FORMAT_UTIL, "layer::write MaterialX network %s\n", parentPath.GetText());

    InputValues inputValues;
    InputConnections inputConnections;
    std::unordered_map<int, SdfPath> uvReaderResultPathMap;
    const InputToMaterialInputTypeMap& remapping =
      ShaderRegistry::getInstance().getOpenPbrInputRemapping();
    auto writeInput = [&](const TfToken& name, const Input& input) {
        if (!input.isEmpty())
            _setupOpenPbrInput(ctx,
                               materialPath,
                               parentPath,
                               name,
                               input,
                               uvReaderResultPathMap,
                               inputValues,
                               inputConnections,
                               remapping,
                               materialInputs);
    };

#define INPUT(x) writeInput(OpenPbrTokens->x, material.x);
    INPUT(base_weight);
    INPUT(base_color);
    INPUT(base_diffuse_roughness);
    INPUT(base_metalness);
    INPUT(specular_weight);
    INPUT(specular_color);
    INPUT(specular_roughness);
    INPUT(specular_ior);
    INPUT(specular_roughness_anisotropy);
    INPUT(transmission_weight);
    INPUT(transmission_color);
    INPUT(transmission_depth);
    INPUT(transmission_scatter);
    INPUT(transmission_scatter_anisotropy);
    INPUT(transmission_dispersion_scale);
    INPUT(transmission_dispersion_abbe_number);
    INPUT(subsurface_weight);
    INPUT(subsurface_color);
    INPUT(subsurface_radius);
    INPUT(subsurface_radius_scale);
    INPUT(subsurface_scatter_anisotropy);
    INPUT(fuzz_weight);
    INPUT(fuzz_color);
    INPUT(fuzz_roughness);
    INPUT(coat_weight);
    INPUT(coat_color);
    INPUT(coat_roughness);
    INPUT(coat_roughness_anisotropy);
    INPUT(coat_ior);
    INPUT(coat_darkening);
    INPUT(thin_film_weight);
    INPUT(thin_film_thickness);
    INPUT(thin_film_ior);
    INPUT(emission_luminance);
    INPUT(emission_color);
    INPUT(geometry_opacity);
    INPUT(geometry_thin_walled);
    INPUT(geometry_normal);
    INPUT(geometry_coat_normal);
    INPUT(geometry_tangent);
    INPUT(geometry_coat_tangent);
#undef INPUT

    // Create OpenPBR surface shader
    SdfPath outputPath = createShader(ctx.sdfData,
                                      parentPath,
                                      MtlXTokens->OpenPBR,
                                      MtlXTokens->ND_open_pbr_surface_surfaceshader,
                                      "out",
                                      inputValues,
                                      inputConnections);
    createShaderOutput(
      ctx.sdfData, materialPath, "mtlx:surface", SdfValueTypeNames->Token, outputPath);

    // TODO: create displacement setup
}
}
