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

#pragma once

#include <unordered_map>

#include "bolson/buffer/allocator.h"

namespace bolson::buffer {

/**
 * \brief Memory allocator for the Intel OPAE FPGA platform.
 *
 * This allocator can currently only allocate exactly one GibiByte.
 */
class OpaeAllocator : public Allocator {
 public:
  // TODO: Temporary work-around for limitations to the OPAE platform.
  static constexpr size_t opae_fixed_capacity = 1024 * 1024 * 1024;

  auto Allocate(size_t size, std::byte **out) -> Status override;
  auto Free(std::byte *buffer) -> Status override;
 private:
  std::unordered_map<void *, size_t> allocations;
};

}
