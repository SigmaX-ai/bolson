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

#include <iostream>

#include <putong/status.h>

namespace flitter {

#define FLITTER_ROE(s) { \
  auto status = s;       \
  if (!s.ok()) return s; \
}

enum class Error {
  GenericError,
  CLIError,
  PulsarError,
  IllexError,
  RapidJSONError,
  IOError
};

using Status = putong::Status<Error>;

}