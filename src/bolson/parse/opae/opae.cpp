// Copyright 2020 Teratide B.V.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bolson/parse/opae/opae.h"

#include <arrow/api.h>
#include <fletcher/common.h>
#include <fletcher/context.h>

#include <memory>

namespace bolson::parse::opae {

auto input_schema() -> std::shared_ptr<arrow::Schema> {
  static auto result = fletcher::WithMetaRequired(
      *arrow::schema({arrow::field("input", arrow::uint8(), false)}), "input",
      fletcher::Mode::READ);
  return result;
}

auto ExtractAddrMap(fletcher::Context* context)
    -> std::unordered_map<const std::byte*, da_t> {
  AddrMap result;
  SPDLOG_DEBUG("TripParser | OPAE host address / device address map:");

  // Workaround to obtain buffer device address.
  for (size_t i = 0; i < context->num_buffers(); i++) {
    const auto* ha =
        reinterpret_cast<const std::byte*>(context->device_buffer(i).host_address);
    auto da = context->device_buffer(i).device_address;
    result[ha] = da;
    SPDLOG_DEBUG("  H: 0x{:016X} <--> D: 0x{:016X}", reinterpret_cast<uint64_t>(ha), da);
  }

  return result;
}

}  // namespace bolson::parse::opae