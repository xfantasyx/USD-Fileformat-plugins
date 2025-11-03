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
#include <fileformatutils/test.h>
#include <gtest/gtest.h>

#include <fileformatutils/layerWriteSdfData.h>
#include <fileformatutils/layerWriteShared.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/sdf/data.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>

#include <filesystem>
#include <fstream>
#include <iostream>

// Run with this turned on to (re-)generate the baselines
#define UPDATE_USDA_BASELINES 0

#if UPDATE_USDA_BASELINES
#    define ASSERT_USDA(usdaLayer, baselinePath)                                                   \
        {                                                                                          \
            std::cout << "Updating USDA baseline " << baselinePath << std::endl;                   \
            usdaLayer->Export(baselinePath);                                                       \
            assertUsda(usdaLayer, baselinePath);                                                   \
        }
#else
#    define ASSERT_USDA(usdaLayer, baselinePath) assertUsda(usdaLayer, baselinePath)
#endif

PXR_NAMESPACE_USING_DIRECTIVE

using namespace adobe::usd;

// This class is here to expose the protected SdfFileFormat::_SetLayerData function to this test
class TestFileFormat : public SdfFileFormat
{
  public:
    static void SetLayerData(SdfLayer* layer, SdfAbstractDataRefPtr& data)
    {
        SdfFileFormat::_SetLayerData(layer, data);
    }
};

void
assertUsda(const SdfLayerHandle& sdfLayer, const std::string& baselinePath)
{
    ASSERT_TRUE(sdfLayer);
    SdfLayerRefPtr baselineLayer = SdfLayer::FindOrOpen(baselinePath);
    ASSERT_TRUE(baselineLayer) << "Failed to load baseline layer from " << baselinePath;

    std::string layerStr;
    sdfLayer->ExportToString(&layerStr);
    std::string baselineStr;
    baselineLayer->ExportToString(&baselineStr);

    if (layerStr != baselineStr) {
        EXPECT_TRUE(false) << "Output of layer " << sdfLayer->GetIdentifier()
                           << " does not match baseline " << baselinePath;

        std::cout << "Layer output has length: " << layerStr.size()
                  << "\nBaseline has length: " << baselineStr.size() << std::endl;

        std::filesystem::path basePath(baselinePath);
        std::string dumpPath = basePath.filename().string();
        std::fstream out(dumpPath, std::ios::out);
        out << layerStr;
        out.close();
        std::cout << "Output dumped to " << dumpPath << std::endl;

        // Very poor person's diff operation. Can we do better without bringing
        // in a diff library?
        for (size_t i = 0; i < layerStr.size(); ++i) {
            if (i >= baselineStr.size()) {
                std::cout << "Size difference. Output has more characters than baseline"
                          << std::endl;
                break;
            }
            if (layerStr[i] != baselineStr[i]) {
                std::cout << "Mismatch at char " << i << std::endl;
                std::cout << "Remainder in output:\n" << &layerStr[i] << std::endl;
                std::cout << "Remainder in baseline:\n" << &baselineStr[i] << std::endl;
                break;
            }
        }
    }
}

void
fillGeneralTestMaterial(UsdData& data)
{
    Material& m = data.addMaterial().second;
    m.name = "GeneralTestMaterial";
    // Set every input to a constant value
    m.useSpecularWorkflow = Input{ VtValue(1) };
    m.diffuseColor = Input{ VtValue(GfVec3f(1.0f, 2.0f, 3.0f)) };
    m.emissiveColor = Input{ VtValue(GfVec3f(1.0f, 2.0f, 3.0f)) };
    m.specularLevel = Input{ VtValue(0.5f) };
    m.specularColor = Input{ VtValue(GfVec3f(1.0f, 0.0f, 1.0f)) };
    m.normal = Input{ VtValue(GfVec3f(0.33f, 0.33f, 0.33f)) };
    m.normalScale = Input{ VtValue(0.666f) };
    m.metallic = Input{ VtValue(0.22f) };
    m.roughness = Input{ VtValue(0.44f) };
    m.clearcoat = Input{ VtValue(0.55f) };
    m.clearcoatColor = Input{ VtValue(GfVec3f(1.0f, 1.0f, 0.0f)) };
    m.clearcoatRoughness = Input{ VtValue(0.66f) };
    m.clearcoatIor = Input{ VtValue(1.33f) };
    m.clearcoatSpecular = Input{ VtValue(0.88f) };
    m.clearcoatNormal = Input{ VtValue(GfVec3f(0.66f, 0.0f, 0.66f)) };
    m.sheenColor = Input{ VtValue(GfVec3f(0.0f, 1.0f, 1.0f)) };
    m.sheenRoughness = Input{ VtValue(0.99f) };
    m.anisotropyLevel = Input{ VtValue(0.321f) };
    m.anisotropyAngle = Input{ VtValue(0.777f) };
    m.opacity = Input{ VtValue(0.8f) };
    m.opacityThreshold = Input{ VtValue(0.75f) };
    m.displacement = Input{ VtValue(1.23f) };
    m.occlusion = Input{ VtValue(0.01f) };
    m.ior = Input{ VtValue(1.55f) };
    m.transmission = Input{ VtValue(0.123f) };
    m.volumeThickness = Input{ VtValue(0.987f) };
    m.absorptionDistance = Input{ VtValue(111.0f) };
    m.absorptionColor = Input{ VtValue(GfVec3f(0.25f, 0.5f, 1.0f)) };
    m.scatteringDistance = Input{ VtValue(222.0f) };
    m.scatteringColor = Input{ VtValue(GfVec3f(1.0f, 0.5f, 1.0f)) };
}

void
fillTextureTestMaterial(UsdData& data)
{
    // Add some images to use
    auto [colorId, colorImage] = data.addImage();
    colorImage.name = "color.png";
    colorImage.uri = "textures/color.png";
    colorImage.format = ImageFormat::ImageFormatPng;
    auto [normalId, normalImage] = data.addImage();
    normalImage.name = "normal.png";
    normalImage.uri = "textures/normal.png";
    normalImage.format = ImageFormat::ImageFormatPng;
    auto [greyscaleId, greyscaleImage] = data.addImage();
    greyscaleImage.name = "greyscale.png";
    greyscaleImage.uri = "textures/greyscale.png";
    greyscaleImage.format = ImageFormat::ImageFormatPng;

    Material& m = data.addMaterial().second;
    m.name = "TextureTestMaterial";
    // Set different inputs to specific texture setups

    // Color textures
    {
        Input input;
        input.image = colorId;
        input.channel = AdobeTokens->rgb;
        input.colorspace = AdobeTokens->sRGB;
        m.diffuseColor = input;

        // Wrap mode, scale & bias, UV transform
        input.wrapS = AdobeTokens->clamp;
        input.wrapT = AdobeTokens->mirror;
        input.scale = GfVec4f(1.0f, 2.0f, 0.5, 1.0f);
        input.bias = GfVec4f(0.1f, 0.2f, 0.3f, 0.0f);
        input.uvRotation = 15.0f;
        input.uvScale = GfVec2f(1.5f, 0.75f);
        input.uvTranslation = GfVec2f(0.12f, 3.45f);
        m.emissiveColor = input;
    }

    // Normal maps
    {
        Input input;
        input.image = normalId;
        input.channel = AdobeTokens->rgb;
        input.colorspace = AdobeTokens->raw;
        m.normal = input;
        m.clearcoatNormal = input;
    }

    // Greyscale maps
    {
        Input input;
        input.image = greyscaleId;
        input.channel = AdobeTokens->r;
        input.colorspace = AdobeTokens->raw;
        m.roughness = input;
    }

    // Single channel from RGB map
    {
        Input input;
        input.image = colorId;
        input.channel = AdobeTokens->g;
        m.clearcoat = input;
    }
}

void
fillTransmissionMaterial(UsdData& data)
{
    Material& m = data.addMaterial().second;
    m.name = "TransmissionTestMaterial";

    // Set transmission, but not opacity. For UsdPreviewSurface this should be mapped as an inverse
    // to opacity
    m.transmission = Input{ VtValue(0.543f) };
}

TEST(FileFormatUtilsTests, writeUsdPreviewSurface)
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("Scene.usda");
    SdfAbstractDataRefPtr sdfData(new SdfData());
    UsdData data;

    fillGeneralTestMaterial(data);
    fillTextureTestMaterial(data);
    fillTransmissionMaterial(data);

    WriteLayerOptions options;
    options.writeUsdPreviewSurface = true;
    options.writeASM = false;
    options.writeOpenPBR = false;

    writeLayer(
      options, data, &*layer, sdfData, "Test Data", "Testing", TestFileFormat::SetLayerData);
    // Clear the doc string, since it contains the date and version number and hence would have to
    // be updated all the time
    layer->SetDocumentation("");

    ASSERT_USDA(layer, "data/baseline_writeUsdPreviewSurface.usda");
}

#ifdef USD_FILEFORMATS_ENABLE_ASM
TEST(FileFormatUtilsTests, writeASM)
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("Scene.usda");
    SdfAbstractDataRefPtr sdfData(new SdfData());
    UsdData data;

    fillGeneralTestMaterial(data);
    fillTextureTestMaterial(data);
    fillTransmissionMaterial(data);

    WriteLayerOptions options;
    options.writeUsdPreviewSurface = false;
    options.writeASM = true;
    options.writeOpenPBR = false;

    writeLayer(
      options, data, &*layer, sdfData, "Test Data", "Testing", TestFileFormat::SetLayerData);
    // Clear the doc string, since it contains the date and version number and hence would have to
    // be updated all the time
    layer->SetDocumentation("");

    ASSERT_USDA(layer, "data/baseline_writeASM.usda");
}
#endif // USD_FILEFORMATS_ENABLE_ASM

TEST(FileFormatUtilsTests, writeOpenPBR)
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("Scene.usda");
    SdfAbstractDataRefPtr sdfData(new SdfData());
    UsdData data;

    fillGeneralTestMaterial(data);
    fillTextureTestMaterial(data);
    fillTransmissionMaterial(data);

    WriteLayerOptions options;
    options.writeUsdPreviewSurface = false;
    options.writeASM = false;
    options.writeOpenPBR = true;

    writeLayer(
      options, data, &*layer, sdfData, "Test Data", "Testing", TestFileFormat::SetLayerData);
    // Clear the doc string, since it contains the date and version number and hence would have to
    // be updated all the time
    layer->SetDocumentation("");

    ASSERT_USDA(layer, "data/baseline_writeOpenPBR.usda");
}