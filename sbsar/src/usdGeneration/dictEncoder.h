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

#include <iostream>
#include <pxr/base/vt/dictionary.h>

namespace adobe::usd::sbsar {
namespace DictEncoder {

/// \brief Serializes a VtDictionary to the given output stream.
/// \param dict The VtDictionary to serialize.
/// \param output The output stream to write the serialized dictionary to.
void
writeDict(const PXR_NS::VtDictionary& dict, std::ostream& output);

/// \brief Deserializes a VtDictionary from the given input stream.
/// \param input The input stream to read the serialized dictionary from.
/// \return The deserialized VtDictionary.
PXR_NS::VtDictionary
readDict(std::istream& input);

}
}
