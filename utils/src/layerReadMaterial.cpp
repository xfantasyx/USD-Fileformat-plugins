/*
Copyright 2025 Adobe. All rights reserved.
This file is licensed to you under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License. You may obtain a copy
of the License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed under
the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR REPRESENTATIONS
OF ANY KIND, either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/
#include <fileformatutils/layerReadMaterial.h>

#include <fileformatutils/common.h>
#include <fileformatutils/debugCodes.h>
#include <fileformatutils/images.h>
#include <fileformatutils/layerWriteShared.h>

#include <pxr/base/tf/pathUtils.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/output.h>
#include <pxr/usd/usdShade/tokens.h>

using namespace PXR_NS;

namespace adobe::usd {

// Populates the absolute path, base name, and sanitized extension for an SBSAR asset by resolving
// the absolute path from the provided URI.
void
populatePathPartsFromAssetPath(const SdfAssetPath& path,
                               std::string& resolvedAssetPath,
                               std::string& name,
                               std::string& extension)
{
    // Make sure we have a resolved path, either coming from SdfAssetPath value or by running it
    // throught the resolver.
    resolvedAssetPath = path.GetResolvedPath().empty()
                          ? ArGetResolver().Resolve(path.GetAssetPath())
                          : path.GetResolvedPath();
    // This will extract the inner most path to the asset:
    // path/to/package.usdz[path/to/image.png] -> path/to/image.png
    std::string innerAssetPath = getLayerFilePath(resolvedAssetPath);
    // This helper function will detect "funky" paths, like those to SBSAR images and convert them
    // to good usable file paths
    std::string filePath = extractFilePathFromAssetPath(innerAssetPath);
    // Strip the path part since we only want the filename and the extension
    std::string baseName = TfGetBaseName(filePath);
    name = TfStringGetBeforeSuffix(baseName);
    extension = TfGetExtension(baseName);
}

bool
readImage(ReadLayerContext& ctx, const SdfAssetPath& assetPath, int& index)
{
    std::string resolvedAssetPath, name, extension;
    populatePathPartsFromAssetPath(assetPath, resolvedAssetPath, name, extension);

    // Check in the cache if we've processed this image before
    if (const auto& it = ctx.images.find(resolvedAssetPath); it != ctx.images.end()) {
        index = it->second;
        TF_DEBUG_MSG(FILE_FORMAT_UTIL,
                     "%s: Image (cached): %s\n",
                     ctx.debugTag.c_str(),
                     resolvedAssetPath.c_str());
        return true;
    }

    // The image is new. Make sure we don't get name collisions in the short name
    if (const auto& itName = ctx.imageNames.find(name); itName != ctx.imageNames.end()) {
        itName->second++;
        name += "_" + std::to_string(itName->second);
        TF_DEBUG_MSG(FILE_FORMAT_UTIL,
                     "%s: Deduplicated image name: %s\n",
                     ctx.debugTag.c_str(),
                     name.c_str());
    } else {
        ctx.imageNames[name] = 1;
    }

    auto [imageIndex, image] = ctx.usd->addImage();
    if (extension == "sbsarimage") {
        // SBSAR images are a special cases where the data is stored raw and must be transcoded to a
        // different image in memory
        extension = getSbsarImageExtension(resolvedAssetPath);
        image.uri = name + "." + extension;
        transcodeImageAssetToMemory(resolvedAssetPath, image.uri, image.image);
    } else {
        auto asset = ArGetResolver().OpenAsset(ArResolvedPath(resolvedAssetPath));
        if (!asset) {
            TF_WARN(
              "%s: Unable to open asset: %s\n", ctx.debugTag.c_str(), resolvedAssetPath.c_str());
            return false;
        }
        image.uri = name + "." + extension;
        image.image.resize(asset->GetSize());
        memcpy(image.image.data(), asset->GetBuffer().get(), asset->GetSize());
    }

    image.name = name;
    image.format = getFormat(extension);
    ctx.images[resolvedAssetPath] = imageIndex;
    index = imageIndex;

    TF_DEBUG_MSG(FILE_FORMAT_UTIL,
                 "%s: Image (new): index: %d uri: %s\n",
                 ctx.debugTag.c_str(),
                 imageIndex,
                 resolvedAssetPath.c_str());

    return true;
}

void
applyInputMult(Input& input, float mult)
{
    if (mult == 1.0f) {
        return;
    }

    if (input.image != -1) {
        input.scale *= mult;
    } else if (input.value.IsHolding<GfVec3f>()) {
        GfVec3f v = input.value.UncheckedGet<GfVec3f>();
        v *= mult;
        input.value = v;
    } else if (input.value.IsHolding<float>()) {
        float v = input.value.UncheckedGet<float>();
        v *= mult;
        input.value = v;
    }
}

template<typename T>
bool
getShaderInputValue(const UsdShadeShader& shader, const TfToken& name, T& value)
{
    UsdShadeInput input = shader.GetInput(name);
    if (input) {
        UsdShadeAttributeVector valueAttrs = input.GetValueProducingAttributes();
        if (!valueAttrs.empty()) {
            const UsdAttribute& attr = valueAttrs.front();
            if (UsdShadeUtils::GetType(attr.GetName()) == UsdShadeAttributeType::Input) {
                valueAttrs.front().Get(&value);
                return true;
            }
        }
    }
    return false;
}

// Fetches the first value-producing attribute connected to a given shader input.
// If 'expectShader' is true, verify that the connected source is a shader and that the connection
// exists. Returns true and sets outAttribute if a suitable attribute is found.
bool
fetchPrimaryConnectedAttribute(const UsdShadeInput& shadeInput,
                               UsdAttribute& outAttribute,
                               bool expectShader)
{
    if (expectShader) {
        if (!shadeInput.HasConnectedSource()) {
            TF_WARN("Input %s has no connected source.", shadeInput.GetFullName().GetText());
            return false;
        }
    }
    UsdShadeAttributeVector attrs = shadeInput.GetValueProducingAttributes();
    if (attrs.empty()) {
        return false;
    }
    if (attrs.size() > 1) {
        TF_WARN("Input %s is connected to multiple producing attributes, only the first will be "
                "processed.",
                shadeInput.GetFullName().GetText());
    }
    outAttribute = attrs[0];
    if (expectShader) {
        UsdShadeAttributeType attrType = UsdShadeUtils::GetType(outAttribute.GetName());
        if (attrType == UsdShadeAttributeType::Input) {
            TF_WARN("Input %s is connected to an attribute that is not a shader.",
                    shadeInput.GetFullName().GetText());
            return false;
        }
    }
    return true;
}

// Handle texture-related shader inputs such as file paths and wrapping modes.
void
handleTextureShader(ReadLayerContext& ctx, const UsdShadeShader& shader, Input& input)
{
    SdfAssetPath assetPath;
    if (getShaderInputValue(shader, AdobeTokens->file, assetPath)) {
        readImage(ctx, assetPath, input.image);
    }
    getShaderInputValue(shader, AdobeTokens->wrapS, input.wrapS);
    getShaderInputValue(shader, AdobeTokens->wrapT, input.wrapT);
    getShaderInputValue(shader, AdobeTokens->minFilter, input.minFilter);
    getShaderInputValue(shader, AdobeTokens->magFilter, input.magFilter);
    getShaderInputValue(shader, AdobeTokens->scale, input.scale);
    getShaderInputValue(shader, AdobeTokens->bias, input.bias);
    getShaderInputValue(shader, AdobeTokens->sourceColorSpace, input.colorspace);

    // Default to 0th UVs unless overridden in handlePrimvarReader
    input.uvIndex = 0;
}

UsdShadeShader
handleTransformShader(ReadLayerContext& ctx, const UsdShadeShader& shader, Input& input)
{

    UsdShadeShader nextShader;
    getShaderInputValue(shader, AdobeTokens->rotation, input.uvRotation);
    getShaderInputValue(shader, AdobeTokens->scale, input.uvScale);
    getShaderInputValue(shader, AdobeTokens->translation, input.uvTranslation);

    UsdShadeInput stInputCoordReader = shader.GetInput(AdobeTokens->in);
    UsdAttribute stSourcesInner;
    if (fetchPrimaryConnectedAttribute(stInputCoordReader, stSourcesInner, true)) {
        nextShader = UsdShadeShader(stSourcesInner.GetPrim());
    }
    return nextShader;
}

void
handlePrimvarReader(ReadLayerContext& ctx, const UsdShadeShader& shader, Input& input)
{
    TfToken texCoordPrimvar;
    std::string texCoordPrimvarStr;
    getShaderInputValue(shader, AdobeTokens->varname, texCoordPrimvarStr);

    // Supports both string and token type values for the varname
    // string is the correct type, but token was added to support slightly
    // incorrect assets.
    if (!texCoordPrimvarStr.empty()) {
        texCoordPrimvar = TfToken(texCoordPrimvarStr);
    } else {
        getShaderInputValue(shader, AdobeTokens->varname, texCoordPrimvar);
    }
    int uvIndex = getSTPrimvarTokenIndex(texCoordPrimvar);
    if (uvIndex >= 0) {
        input.uvIndex = uvIndex;
    } else {
        TF_WARN("Texture reader %s is reading primvar %s. Only 'st' or 'st1'..'stN' is supported",
                shader.GetPrim().GetPath().GetText(),
                texCoordPrimvar.GetText());
    }
}

void
readInput(ReadLayerContext& ctx, const UsdShadeShader& surface, const TfToken& name, Input& input)
{
    UsdShadeInput shadeInput = surface.GetInput(name);
    if (!shadeInput) {
        return;
    }

    UsdAttribute attr;
    if (fetchPrimaryConnectedAttribute(shadeInput, attr, false)) {
        UsdShadeSourceInfoVector sources = shadeInput.GetConnectedSources();

        // Attempt to retrieve the constant value from the attribute.
        auto [shadingAttrName, attrType] = UsdShadeUtils::GetBaseNameAndType(attr.GetName());
        if (attrType == UsdShadeAttributeType::Input) {
            if (!attr.Get(&input.value)) {
                TF_WARN("Failed to get constant value for input %s", name.GetText());
                return;
            }
        } else {
            // Process the shader connected to this attribute
            UsdShadeShader connectedShader(attr.GetPrim());
            TfToken shaderId;
            connectedShader.GetShaderId(&shaderId);

            if (shaderId == AdobeTokens->UsdUVTexture) {
                handleTextureShader(ctx, connectedShader, input);

                UsdShadeInput stInput = connectedShader.GetInput(AdobeTokens->st);

                // The name of the output on the texture reader determines which channel(s) of the
                // texture we read.
                input.channel = shadingAttrName;

                // Process the connected source of the 'st' input.
                if (fetchPrimaryConnectedAttribute(stInput, attr, true)) {
                    VtValue srcValue;
                    if (attr.Get(&srcValue)) {
                        TF_WARN(
                          "Texture read shader does not support a fixed UV value for input %s",
                          name.GetText());
                    } else {
                        // Handle the shader connected to the UV coordinate.
                        UsdShadeShader stShader(attr.GetPrim());
                        stShader.GetShaderId(&shaderId);

                        if (shaderId == AdobeTokens->UsdTransform2d) {
                            UsdShadeShader nextShader = handleTransformShader(ctx, stShader, input);
                            if (nextShader) {
                                stShader = nextShader;
                                stShader.GetShaderId(&shaderId);
                            }
                        }

                        // This is not an "else if", since we can move the stShader
                        // if we encounter a UV transform.
                        if (shaderId == AdobeTokens->UsdPrimvarReader_float2) {
                            handlePrimvarReader(ctx, stShader, input);
                        } else {
                            TF_WARN("Unsupported shader type %s for UV input %s",
                                    shaderId.GetText(),
                                    name.GetText());
                        }
                    }
                } else {
                    TF_WARN("Failed to fetch connected attribute for UV input %s", name.GetText());
                }
            } else {
                TF_WARN(
                  "Unsupported shader type %s for input %s", shaderId.GetText(), name.GetText());
            }
        }
    } else {
        // If no connections were found, get the shader's input value directly
        if (!getShaderInputValue(surface, name, input.value)) {
            TF_WARN("Failed to get input value for %s", name.GetText());
        }
    }
}

bool
readUsdPreviewSurfaceMaterial(ReadLayerContext& ctx,
                              Material& material,
                              const UsdShadeShader& surface)
{
    TfToken infoIdToken;
    surface.GetShaderId(&infoIdToken);
    if (infoIdToken != AdobeTokens->UsdPreviewSurface) {
        return false;
    }

    readInput(
      ctx, surface, UsdPreviewSurfaceTokens->useSpecularWorkflow, material.useSpecularWorkflow);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->diffuseColor, material.diffuseColor);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->emissiveColor, material.emissiveColor);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->specularColor, material.specularColor);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->normal, material.normal);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->metallic, material.metallic);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->roughness, material.roughness);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->clearcoat, material.clearcoat);
    readInput(
      ctx, surface, UsdPreviewSurfaceTokens->clearcoatRoughness, material.clearcoatRoughness);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->opacity, material.opacity);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->opacityThreshold, material.opacityThreshold);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->displacement, material.displacement);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->occlusion, material.occlusion);
    readInput(ctx, surface, UsdPreviewSurfaceTokens->ior, material.ior);

    return true;
}

bool
_readClearcoatModelsTransmissionTint(const UsdShadeShader& surface)
{
    bool value = false;
    // Check for a custom attribute that carries an indicator where the clearcoat came from
    surface.GetPrim().GetAttribute(AdobeTokens->clearcoatModelsTransmissionTint).Get(&value);
    return value;
}

bool
_readUnlit(const UsdShadeShader& surface)
{
    bool value = false;
    // Check for a custom attribute that carries an indicator where the clearcoat came from
    surface.GetPrim().GetAttribute(AdobeTokens->unlit).Get(&value);
    return value;
}

bool
readASMMaterial(ReadLayerContext& ctx, Material& material, const UsdShadeShader& surface)
{
    TfToken infoIdToken;
    surface.GetShaderId(&infoIdToken);
    if (infoIdToken != AdobeTokens->adobeStandardMaterial) {
        return false;
    }

    material.clearcoatModelsTransmissionTint = _readClearcoatModelsTransmissionTint(surface);
    material.isUnlit = _readUnlit(surface);

    // Note, we currently only support fixed values for emissiveIntensity and sheenOpacity
    // No texture support yet.
    float emissiveIntensity = 0.0f;
    float sheenOpacity = 0.0f;
    bool scatter = false;

    auto getConstShaderInput = [&](const TfToken& inputName, auto& var) {
        VtValue val;
        if (getShaderInputValue(surface, inputName, val)) {
            if (val.IsHolding<std::remove_reference_t<decltype(var)>>()) {
                var = val.UncheckedGet<std::remove_reference_t<decltype(var)>>();
            }
        }
    };

    getConstShaderInput(AsmTokens->emissiveIntensity, emissiveIntensity);
    getConstShaderInput(AsmTokens->sheenOpacity, sheenOpacity);
    getConstShaderInput(AsmTokens->scatter, scatter);

    readInput(ctx, surface, AsmTokens->baseColor, material.diffuseColor);
    readInput(ctx, surface, AsmTokens->roughness, material.roughness);
    readInput(ctx, surface, AsmTokens->metallic, material.metallic);
    readInput(ctx, surface, AsmTokens->opacity, material.opacity);
    // Note, this is a specially supported attribute from UsdPreviewSurface that we transport via
    // ASM, so that we do not loose this information
    readInput(ctx, surface, UsdPreviewSurfaceTokens->opacityThreshold, material.opacityThreshold);
    readInput(ctx, surface, AsmTokens->specularLevel, material.specularLevel);
    readInput(ctx, surface, AsmTokens->specularEdgeColor, material.specularColor);
    readInput(ctx, surface, AsmTokens->normal, material.normal);
    readInput(ctx, surface, AsmTokens->normalScale, material.normalScale);
    readInput(ctx, surface, AsmTokens->height, material.displacement);
    readInput(ctx, surface, AsmTokens->anisotropyLevel, material.anisotropyLevel);
    readInput(ctx, surface, AsmTokens->anisotropyAngle, material.anisotropyAngle);
    if (emissiveIntensity > 0.0f) {
        readInput(ctx, surface, AsmTokens->emissive, material.emissiveColor);
        applyInputMult(material.emissiveColor, emissiveIntensity);
    }
    if (sheenOpacity > 0.0f) {
        readInput(ctx, surface, AsmTokens->sheenColor, material.sheenColor);
        // XXX sheenOpacity can't really be multiplied into the color. We currently drop this value
    }
    readInput(ctx, surface, AsmTokens->sheenRoughness, material.sheenRoughness);
    readInput(ctx, surface, AsmTokens->translucency, material.transmission);
    readInput(ctx, surface, AsmTokens->IOR, material.ior);
    readInput(ctx, surface, AsmTokens->absorptionColor, material.absorptionColor);
    readInput(ctx, surface, AsmTokens->absorptionDistance, material.absorptionDistance);
    if (scatter) {
        readInput(ctx, surface, AsmTokens->scatteringColor, material.scatteringColor);
        readInput(ctx, surface, AsmTokens->scatteringDistance, material.scatteringDistance);
        readInput(ctx, surface, AsmTokens->scatteringDistanceScale, material.scatteringDistanceScale);
    }
    readInput(ctx, surface, AsmTokens->coatOpacity, material.clearcoat);
    readInput(ctx, surface, AsmTokens->coatColor, material.clearcoatColor);
    readInput(ctx, surface, AsmTokens->coatRoughness, material.clearcoatRoughness);
    readInput(ctx, surface, AsmTokens->coatIOR, material.clearcoatIor);
    readInput(ctx, surface, AsmTokens->coatSpecularLevel, material.clearcoatSpecular);
    readInput(ctx, surface, AsmTokens->coatNormal, material.clearcoatNormal);
    readInput(ctx, surface, AsmTokens->ambientOcclusion, material.occlusion);
    readInput(ctx, surface, AsmTokens->volumeThickness, material.volumeThickness);

    return true;
}

bool
readMaterial(ReadLayerContext& ctx, const UsdPrim& prim, int parent)
{
    auto [materialIndex, material] = ctx.usd->addMaterial();
    ctx.materials[prim.GetPath().GetString()] = materialIndex;
    material.name = prim.GetPath().GetName();
    material.displayName = prim.GetDisplayName();
    UsdShadeMaterial usdMaterial(prim);

    // We give preference to the Adobe ASM surface, if present, and fallback to the standard
    // UsdPreviewSurface
    UsdShadeShader surface = usdMaterial.ComputeSurfaceSource({ AdobeTokens->adobe });
    bool success = false;
    if (surface) {
        success = readASMMaterial(ctx, material, surface);
        if (!success) {
            success = readUsdPreviewSurfaceMaterial(ctx, material, surface);
        }
    } else {
        TF_WARN("No surface shader for material %s", prim.GetPath().GetText());
    }

    printMaterial("layer::read", prim.GetPath(), material, ctx.debugTag);
    return success;
}

}