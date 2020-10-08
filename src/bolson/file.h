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

#include "./pulsar.h"

namespace bolson {

/// Options for the file subcommand.
struct FileOptions {
  /// Pulsar options.
  PulsarOptions pulsar;
  /// Input file path.
  std::string input;
  /// Whether to produce succinct stats.
  bool succinct = false;
};

/**
 * \brief Produce Pulsar messages from a file with JSONs.
 * \param opt Options for the file subcommand.
 * \return Status::OK() if successful, some error otherwise.
 */
auto ProduceFromFile(const FileOptions &opt) -> Status;

}  // namespace bolson