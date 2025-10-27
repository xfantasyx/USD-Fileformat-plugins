/*
Copyright 2024 Adobe. All rights reserved.
This file is licensed to you under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License. You may obtain a copy
of the License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed under
the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR REPRESENTATIONS
OF ANY KIND, either express or implied. See the License for the specific language
governing permissions and limitations under the License.
*/
#pragma once

#include "sbsarSymbolMapper.h"
#include <sbsarfileformat.h>

#include <pxr/pxr.h>
#include <pxr/usd/sdf/abstractData.h>

#include <substance/framework/framework.h>

#include <string>

namespace adobe::usd::sbsar {

/// @brief Add a LuxDomeLight prim to the given Sdf layer.
///
/// The LuxDomeLight prim represents a dome light with an IBL texture.
///
/// @param sdfData          Sdf data to store the layer in.
/// @param graphName        Name of the current sbsar graph.
/// @param graphDesc        Description of the current sbsar graph.
/// @param packagePath      Path of the sbsar file.
/// @param sbsarHash        Hash of the sbsar.
/// @param symbolMapper     Symbol mapper to avoid conflict between parameters.
/// @param sbsarData        Options for the sbsar. See SBSAROptions.
/// @return The path of the created dome light prim.
PXR_NS::SdfPath
addLuxDomeLight(PXR_NS::SdfAbstractData* sdfData,
                const MappedSymbol& graphName,
                const SubstanceAir::GraphDesc& graphDesc,
                const std::string& packagePath,
                size_t sbsarHash,
                SymbolMapper& symbolMapper,
                const SBSAROptions& sbsarData);

}
