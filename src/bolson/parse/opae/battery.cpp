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

#include "bolson/parse/opae/battery.h"

#include <arrow/api.h>
#include <arrow/ipc/api.h>
#include <fletcher/context.h>
#include <fletcher/fletcher.h>
#include <fletcher/kernel.h>
#include <fletcher/platform.h>
#include <putong/timer.h>

#include <CLI/CLI.hpp>
#include <chrono>
#include <thread>
#include <utility>

#include "bolson/latency.h"
#include "bolson/log.h"
#include "bolson/parse/opae/opae.h"
#include "bolson/parse/parser.h"

namespace bolson::parse::opae {

auto BatteryParserContext::PrepareInputBatches(
    const std::vector<illex::JSONBuffer*>& buffers) -> Status {
  for (const auto& buf : buffers) {
    auto wrapped = arrow::Buffer::Wrap(buf->data(), buf->capacity());
    auto array =
        std::make_shared<arrow::PrimitiveArray>(arrow::uint8(), buf->capacity(), wrapped);
    batches_in.push_back(
        arrow::RecordBatch::Make(opae::input_schema(), buf->capacity(), {array}));
  }
  return Status::OK();
}

auto BatteryParserContext::PrepareOutputBatches() -> Status {
  for (size_t i = 0; i < num_parsers_; i++) {
    std::byte* offsets = nullptr;
    std::byte* values = nullptr;
    BOLSON_ROE(allocator.Allocate(allocator.fixed_capacity(), &offsets));
    BOLSON_ROE(allocator.Allocate(allocator.fixed_capacity(), &values));

    auto offset_buffer = arrow::Buffer::Wrap(offsets, allocator.fixed_capacity());
    auto values_buffer = arrow::Buffer::Wrap(values, allocator.fixed_capacity());
    auto values_array =
        std::make_shared<arrow::PrimitiveArray>(arrow::uint64(), 0, values_buffer);
    auto list_array = std::make_shared<arrow::ListArray>(BatteryParser::output_type(), 0,
                                                         offset_buffer, values_array);
    std::vector<std::shared_ptr<arrow::Array>> arrays = {list_array};
    raw_out_offsets.push_back(offsets);
    raw_out_values.push_back(values);
    batches_out.push_back(
        arrow::RecordBatch::Make(BatteryParser::output_schema(), 0, arrays));
  }

  return Status::OK();
}

auto BatteryParserContext::Make(const BatteryOptions& opts,
                                std::shared_ptr<ParserContext>* out) -> Status {
  // Attempt to derive AFU ID if not supplied.
  std::string afu_id_str;
  if (opts.afu_id.empty()) {
    if (opts.num_parsers > 255) {
      return Status(
          Error::OpaeError,
          "Auto-deriving AFU ID for number of parsers larger than 255 is not supported.");
    }
    std::stringstream ss;
    ss << std::setw(2) << std::setfill('0') << std::hex << opts.num_parsers;
    // AFU ID from the hardware design script.
    afu_id_str = "9ca43fb0-c340-4908-b79b-5c89b4ef5e" + ss.str();
  } else {
    afu_id_str = opts.afu_id;
  }
  SPDLOG_DEBUG("BatteryParserManager | Using AFU ID: {}", afu_id_str);

  // Create and set up result.
  auto result = std::make_shared<BatteryParserContext>();
  result->opts_ = opts;
  result->num_parsers_ = opts.num_parsers;

  SPDLOG_DEBUG("BatteryParserManager | Setting up for {} parsers.", opts.num_parsers);

  FLETCHER_ROE(fletcher::Platform::Make("opae", &result->platform, false));

  result->opts_.afu_id = afu_id_str;
  char* afu_id = result->opts_.afu_id.data();
  result->platform->init_data = &afu_id;

  // Initialize the platform.
  FLETCHER_ROE(result->platform->Init());

  *out = result;

  return Status::OK();
}

auto BatteryParserContext::PrepareParsers() -> Status {
  for (size_t i = 0; i < num_parsers_; i++) {
    parsers_.push_back(std::make_shared<BatteryParser>(
        platform.get(), context.get(), kernel.get(), &h2d_addr_map, i, num_parsers_,
        raw_out_offsets[i], raw_out_values[i], &platform_mutex));
  }
  return Status::OK();
}

auto BatteryParserContext::parsers() -> std::vector<std::shared_ptr<Parser>> {
  return CastPtrs<Parser>(parsers_);
}

auto BatteryParserContext::CheckThreadCount(size_t num_threads) const -> size_t {
  return parsers_.size();
}

auto BatteryParserContext::CheckBufferCount(size_t num_buffers) const -> size_t {
  return parsers_.size();
}

auto BatteryParserContext::Init(const std::vector<illex::JSONBuffer*>& buffers)
    -> Status {
  if (parsers_.size() != buffers.size()) {
    return Status(Error::OpaeError,
                  "BatteryParser implementation requires number of buffers and "
                  "parsers to be equal.");
  }

  // Pull everything through the fletcher stack once.
  FLETCHER_ROE(fletcher::Context::Make(&context, platform));

  BOLSON_ROE(PrepareInputBatches(buffers));
  BOLSON_ROE(PrepareOutputBatches());

  for (const auto& batch : batches_in) {
    FLETCHER_ROE(context->QueueRecordBatch(batch));
  }

  for (const auto& batch : batches_out) {
    FLETCHER_ROE(context->QueueRecordBatch(batch));
  }

  // Enable context.
  FLETCHER_ROE(context->Enable());
  // Construct kernel handler.
  kernel = std::make_shared<fletcher::Kernel>(context);
  // Write metadata.
  FLETCHER_ROE(kernel->WriteMetaData());

  SPDLOG_DEBUG("BatteryParserManager | OPAE host address / device address map:");

  // Workaround to obtain buffer device address.
  for (size_t i = 0; i < context->num_buffers(); i++) {
    const auto* ha =
        reinterpret_cast<const std::byte*>(context->device_buffer(i).host_address);
    auto da = context->device_buffer(i).device_address;
    h2d_addr_map[ha] = da;
    SPDLOG_DEBUG("  H: 0x{:016X} <--> D: 0x{:016X}", reinterpret_cast<uint64_t>(ha), da);
  }

  SPDLOG_DEBUG("BatteryParserManager | Preparing parsers.");

  BOLSON_ROE(PrepareParsers());

  return Status::OK();
}

auto BatteryParserContext::schema() const -> std::shared_ptr<arrow::Schema> {
  return BatteryParser::output_schema();
}

static auto WrapOutput(int32_t num_rows, uint8_t* offsets, uint8_t* values,
                       std::shared_ptr<arrow::Schema> schema,
                       std::shared_ptr<arrow::RecordBatch>* out) -> Status {
  auto ret = Status::OK();

  // +1 because the last value in offsets buffer is the next free index in the values
  // buffer.
  int32_t num_offsets = num_rows + 1;

  // Obtain the last value in the offsets buffer to know how many values there are.
  int32_t num_values = (reinterpret_cast<int32_t*>(offsets))[num_rows];

  size_t num_offset_bytes = num_offsets * sizeof(int32_t);
  size_t num_values_bytes = num_values * sizeof(uint64_t);

  try {
    auto values_buf = arrow::Buffer::Wrap(values, num_values_bytes);
    auto offsets_buf = arrow::Buffer::Wrap(offsets, num_offset_bytes);
    auto value_array =
        std::make_shared<arrow::PrimitiveArray>(arrow::uint64(), num_values, values_buf);
    auto offsets_array =
        std::make_shared<arrow::PrimitiveArray>(arrow::int32(), num_offsets, offsets_buf);
    auto list_array = arrow::ListArray::FromArrays(*offsets_array, *value_array);

    std::vector<std::shared_ptr<arrow::Array>> arrays = {list_array.ValueOrDie()};
    *out = arrow::RecordBatch::Make(std::move(schema), num_rows, arrays);
  } catch (std::exception& e) {
    return Status(Error::ArrowError, e.what());
  }

  return Status::OK();
}

auto BatteryParser::ParseOne(illex::JSONBuffer* in, ParsedBatch* out) -> Status {
  platform_mutex->lock();
  auto* p = platform_;
  SPDLOG_DEBUG("Thread {:2} | Obtained platform lock", idx_);
  SPDLOG_DEBUG("Thread {:2} | Attempting to parse buffer:\n {}", idx_,
               ToString(*in, false));

  // Reset the kernel, start it, and poll until completion.
  // FLETCHER_ROE(kernel_->Reset());
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), ctrl_reset, idx_, "ctrl"));
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), 0, idx_, "ctrl"));

  // rewrite the input last index because of opae limitations.
  BOLSON_ROE(
      WriteMMIO(p, input_lastidx_offset(idx_), in->size(), idx_, "input last idx"));

  dau_t input_addr;
  input_addr.full = h2d_addr_map->at(in->data());

  BOLSON_ROE(WriteMMIO(p, input_values_lo_offset(idx_), input_addr.lo, idx_,
                       "in values addr lo"));
  BOLSON_ROE(WriteMMIO(p, input_values_hi_offset(idx_), input_addr.hi, idx_,
                       "in values addr hi"));

  // FLETCHER_ROE(kernel_->Start());
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), ctrl_start, idx_, "ctrl"));
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), 0, idx_, "ctrl"));

  // FLETCHER_ROE(kernel_->PollUntilDone());
  bool done = false;
  uint32_t status = 0;
  dau_t num_rows;

  do {
#ifndef NDEBUG
    ReadMMIO(p, status_offset(idx_), &status, idx_, "status");
    done = (status & stat_done) == stat_done;

    // Obtain the result for debugging.
    ReadMMIO(p, result_rows_offset_lo(idx_), &num_rows.lo, idx_, "rows lo");
    ReadMMIO(p, result_rows_offset_hi(idx_), &num_rows.hi, idx_, "rows hi");
    SPDLOG_DEBUG("Thread {:2} | Number of rows: {}", idx_, num_rows.full);
    platform_mutex->unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    platform_mutex->lock();
#else
    platform_->ReadMMIO(status_offset(idx_), &status);
    done = (status & stat_done) == stat_done;

    platform_mutex->unlock();
    std::this_thread::sleep_for(std::chrono::microseconds(BOLSON_QUEUE_WAIT_US));
    platform_mutex->lock();
#endif
  } while (!done);

  ReadMMIO(p, result_rows_offset_lo(idx_), &num_rows.lo, idx_, "rows lo");
  ReadMMIO(p, result_rows_offset_hi(idx_), &num_rows.hi, idx_, "rows hi");
  platform_mutex->unlock();

  std::shared_ptr<arrow::RecordBatch> out_batch;
  BOLSON_ROE(WrapOutput(num_rows.full, reinterpret_cast<uint8_t*>(raw_out_offsets),
                        reinterpret_cast<uint8_t*>(raw_out_values), output_schema(),
                        &out_batch));

  SPDLOG_DEBUG("Thread {:2} | Parsing {} JSONs completed.", idx_, out_batch->num_rows());

  ParsedBatch result(out_batch, in->range());

  *out = result;

  return Status::OK();
}

auto BatteryParser::Parse(const std::vector<illex::JSONBuffer*>& in,
                          std::vector<ParsedBatch>* out) -> Status {
  for (auto* buf : in) {
    ParsedBatch batch;
    BOLSON_ROE(ParseOne(buf, &batch));
    out->push_back(batch);
  }

  return Status::OK();
}

auto BatteryParser::output_type() -> std::shared_ptr<arrow::DataType> {
  static auto result = arrow::list(arrow::field("item", arrow::uint64(), false));
  return result;
}

auto BatteryParser::output_schema() -> std::shared_ptr<arrow::Schema> {
  static auto result = fletcher::WithMetaRequired(
      *arrow::schema({arrow::field("voltage", output_type(), false)}), "output",
      fletcher::Mode::WRITE);
  return result;
}

void AddBatteryOptionsToCLI(CLI::App* sub, BatteryOptions* out) {
  sub->add_option("--battery-afu-id",
                  "OPAE \"battery status\" AFU ID. "
                  "If not supplied, it is derived from number of parser instances.");
  sub->add_option("--battery-num-parsers",
                  "OPAE \"battery status\" number of parser instances.")
      ->default_val(BOLSON_DEFAULT_OPAE_BATTERY_PARSERS);
}

}  // namespace bolson::parse::opae
