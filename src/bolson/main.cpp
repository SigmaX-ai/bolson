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

#include "bolson/log.h"
#include "bolson/cli.h"
#include "bolson/file.h"
#include "bolson/stream.h"
#include "bolson/status.h"

auto main(int argc, char *argv[]) -> int {
  // Set up logger.
  bolson::StartLogger();

  // Handle CLI.
  bolson::AppOptions opts;
  auto status = bolson::AppOptions::FromArguments(argc, argv, &opts);
  if (status.ok()) {
    // Run sub-programs.
    bolson::Status result;
    switch (opts.sub) {
      case bolson::SubCommand::NONE: break;
      case bolson::SubCommand::FILE: status = bolson::ProduceFromFile(opts.file);
      case bolson::SubCommand::STREAM: status = bolson::ProduceFromStream(opts.stream);
    }
  }

  if (!status.ok()) {
    spdlog::error("{} exiting with errors.", bolson::AppOptions::name);
    spdlog::error("  {}", status.msg());
  }

  return 0;
}