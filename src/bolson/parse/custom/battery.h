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

#include <arrow/api.h>
#include <fletcher/api.h>

#include <CLI/CLI.hpp>
#include <memory>
#include <utility>

#include "bolson/parse/parser.h"
#include "bolson/utils.h"

namespace bolson::parse::custom {

struct BatteryOptions {
  /// Number of input buffers to use, when set to 0, it will be equal to the number of
  /// threads.
  size_t num_buffers = 0;
  /// Whether to store sequence numbers as a column.
  bool seq_column = false;
  /// Capacity of input buffers.
  size_t buf_capacity = 0;

  /// Number of values to pre-allocate.
  size_t pre_alloc_values;
  /// Number of offsets to pre-allocate.
  size_t pre_alloc_offsets;
};

void AddBatteryOptionsToCLI(CLI::App* sub, BatteryOptions* out);

class BatteryParser : public Parser {
 public:
  explicit BatteryParser(bool seq_column);

  auto Parse(const std::vector<illex::JSONBuffer*>& in, std::vector<ParsedBatch>* out)
      -> Status override;

  virtual auto ParseOne(const illex::JSONBuffer* buffer, ParsedBatch* out) -> Status;

  static auto input_schema() -> std::shared_ptr<arrow::Schema>;
  [[nodiscard]] auto output_schema() const -> std::shared_ptr<arrow::Schema>;

 protected:
  bool seq_column = false;
  std::shared_ptr<arrow::Schema> output_schema_;
};

class UnsafeBatteryParser : public BatteryParser {
 public:
  explicit UnsafeBatteryParser(bool seq_column, size_t pre_alloc_offsets,
                               size_t pre_alloc_values);

  auto ParseOne(const illex::JSONBuffer* buffer, ParsedBatch* out) -> Status override;

 private:
  size_t pre_alloc_offsets;
  size_t pre_alloc_values;
};

class BatteryParserContext : public ParserContext {
 public:
  static auto Make(const BatteryOptions& opts, size_t num_parsers, size_t input_size,
                   std::shared_ptr<ParserContext>* out) -> Status;

  auto parsers() -> std::vector<std::shared_ptr<Parser>> override;

  [[nodiscard]] auto input_schema() const -> std::shared_ptr<arrow::Schema> override;
  [[nodiscard]] auto output_schema() const -> std::shared_ptr<arrow::Schema> override;

 private:
  std::vector<std::shared_ptr<BatteryParser>> parsers_;
};

}  // namespace bolson::parse::custom
