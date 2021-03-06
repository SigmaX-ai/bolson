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

#include "bolson/parse/fpga/battery.h"

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
#include "bolson/parse/fpga/common.h"
#include "bolson/parse/parser.h"

namespace bolson::parse::fpga {

static auto voltage_type() -> std::shared_ptr<arrow::DataType> {
  static auto result = arrow::list(arrow::field("item", arrow::uint64(), false));
  return result;
}

auto BatteryParserContext::PrepareInputBatches() -> Status {
  for (const auto& buf : buffers_) {
    auto wrapped = arrow::Buffer::Wrap(buf.data(), buf.capacity());
    auto array =
        std::make_shared<arrow::PrimitiveArray>(arrow::uint8(), buf.capacity(), wrapped);
    batches_in.push_back(arrow::RecordBatch::Make(
        bolson::parse::fpga::raw_json_input_schema(), buf.capacity(), {array}));
  }
  return Status::OK();
}

auto BatteryParserContext::PrepareOutputBatches(size_t offsets_cap, size_t values_cap)
    -> Status {
  for (size_t i = 0; i < num_parsers_; i++) {
    std::byte* offsets = nullptr;
    std::byte* values = nullptr;
    BOLSON_ROE(allocator_->Allocate(offsets_cap, &offsets));
    BOLSON_ROE(allocator_->Allocate(values_cap, &values));

    auto offset_buffer = arrow::Buffer::Wrap(offsets, offsets_cap);
    auto values_buffer = arrow::Buffer::Wrap(values, values_cap);
    auto values_array =
        std::make_shared<arrow::PrimitiveArray>(arrow::uint64(), 0, values_buffer);
    auto list_array = std::make_shared<arrow::ListArray>(voltage_type(), 0, offset_buffer,
                                                         values_array);
    std::vector<std::shared_ptr<arrow::Array>> arrays = {list_array};
    raw_out_offsets.push_back(offsets);
    raw_out_values.push_back(values);
    batches_out.push_back(
        arrow::RecordBatch::Make(BatteryParser::output_schema(), 0, arrays));
  }

  return Status::OK();
}

auto BatteryParserContext::Make(const BatteryOptions& opts, size_t input_size,
                                std::shared_ptr<ParserContext>* out) -> Status {
  // Check parameters.
  if (opts.num_parsers > 256) {
    return {Error::FletcherError,
            "Hardware does not allow more than 256 parser instances."};
  }
  // Create and set up result.
  auto result = std::shared_ptr<BatteryParserContext>(new BatteryParserContext(opts));
  SPDLOG_DEBUG("BatteryParserContext | Setting up for {} parsers.", result->num_parsers_);

  // Initialize the platform.
  FLETCHER_ROE(fletcher::Platform::Make(&result->platform, false));
  FLETCHER_ROE(result->platform->Init());

  // Allocate input buffers.
  BOLSON_ROE(result->AllocateBuffers(result->num_parsers_,
                                     DivideCeil(input_size, result->num_parsers_)));

  // Pull everything through the fletcher stack once.
  FLETCHER_ROE(fletcher::Context::Make(&result->context, result->platform));

  BOLSON_ROE(result->PrepareInputBatches());
  BOLSON_ROE(result->PrepareOutputBatches(opts.out_offset_buffer_capacity,
                                          opts.out_values_buffer_capacity));

  for (const auto& batch : result->batches_in) {
    FLETCHER_ROE(result->context->QueueRecordBatch(batch));
  }

  for (const auto& batch : result->batches_out) {
    FLETCHER_ROE(result->context->QueueRecordBatch(batch));
  }

  // Enable context.
  FLETCHER_ROE(result->context->Enable());
  // Construct kernel handler.
  result->kernel = std::make_shared<fletcher::Kernel>(result->context);
  // Write metadata.
  // FLETCHER_ROE(result->kernel->WriteMetaData());

  SPDLOG_DEBUG("BatteryParserContext | Preparing parsers.");
  BOLSON_ROE(result->PrepareParsers());

  // Determine input and output schema.
  result->input_schema_ = BatteryParser::output_schema();
  if (opts.seq_column) {
    BOLSON_ROE(WithSeqField(*BatteryParser::output_schema(), &result->output_schema_));
  } else {
    result->output_schema_ = BatteryParser::output_schema();
  }

  *out = result;

  return Status::OK();
}

auto BatteryParserContext::PrepareParsers() -> Status {
  for (size_t i = 0; i < num_parsers_; i++) {
    auto parser = std::make_shared<BatteryParser>(
        platform.get(), context.get(), kernel.get(), i, num_parsers_, raw_out_offsets[i],
        raw_out_values[i], &platform_mutex, seq_column);
    parser->Init();
    parsers_.push_back(parser);
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

auto BatteryParserContext::input_schema() const -> std::shared_ptr<arrow::Schema> {
  return input_schema_;
}

auto BatteryParserContext::output_schema() const -> std::shared_ptr<arrow::Schema> {
  return output_schema_;
}

BatteryParserContext::BatteryParserContext(const BatteryOptions& opts)
    : num_parsers_(opts.num_parsers), seq_column(opts.seq_column) {
  allocator_ = std::make_shared<buffer::FpgaAllocator>();
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
  SPDLOG_DEBUG("BatteryParser {:2} | Obtained platform lock", idx_);
  SPDLOG_DEBUG("BatteryParser {:2} | Attempting to parse buffer:\n {}", idx_,
               ToString(*in, true));

  // Reset the kernel, start it, and poll until completion.
  // FLETCHER_ROE(kernel_->Reset());
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), ctrl_reset, idx_, "ctrl"));
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), 0, idx_, "ctrl"));

  // Write the input last index, to let the parser know the input buffer size.
  BOLSON_ROE(
      WriteMMIO(p, input_lastidx_offset(idx_), in->size(), idx_, "input last idx"));

  dau_t input_addr;
  input_addr.full = reinterpret_cast<da_t>(in->data());

  BOLSON_ROE(WriteMMIO(p, input_values_lo_offset(idx_), input_addr.lo, idx_,
                       "in values addr lo"));
  BOLSON_ROE(WriteMMIO(p, input_values_hi_offset(idx_), input_addr.hi, idx_,
                       "in values addr hi"));

  // FLETCHER_ROE(kernel_->Start());
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), ctrl_start, idx_, "ctrl"));
  BOLSON_ROE(WriteMMIO(p, ctrl_offset(idx_), 0, idx_, "ctrl"));

  // While FPGA is busy, prepare sequence number column if necessary.
  std::shared_ptr<arrow::UInt64Array> seq;
  if (seq_column) {
    arrow::UInt64Builder builder;
    ARROW_ROE(builder.Reserve(in->range().last - in->range().first + 1));
    for (uint64_t s = in->range().first; s <= in->range().last; s++) {
      builder.UnsafeAppend(s);
    }
    ARROW_ROE(builder.Finish(&seq));
  }

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
    SPDLOG_DEBUG("BatteryParser {:2} | Number of rows: {}", idx_, num_rows.full);
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

  std::shared_ptr<arrow::RecordBatch> final_batch;

  // Prepend the sequence number column that was potentially made earlier.
  if (seq_column) {
    auto final_batch_result = out_batch->AddColumn(0, "bolson_seq", seq);
    if (!final_batch_result.ok()) {
      return Status(Error::ArrowError, final_batch_result.status().message());
    }
    final_batch = final_batch_result.ValueOrDie();
  } else {
    final_batch = AddSeqAsSchemaMeta(out_batch, in->range());
  }

  SPDLOG_DEBUG("BatteryParser {:2} | Parsing {} JSONs completed.", idx_,
               final_batch->num_rows());

  *out = ParsedBatch(final_batch, in->range());

  return Status::OK();
}

auto BatteryParser::Parse(const std::vector<illex::JSONBuffer*>& in,
                          std::vector<ParsedBatch>* out) -> Status {
  for (auto* buf : in) {
    ParsedBatch batch;
    BOLSON_ROE(this->ParseOne(buf, &batch));
    out->push_back(batch);
  }

  return Status::OK();
}

auto BatteryParser::output_schema() -> std::shared_ptr<arrow::Schema> {
  static auto result = ::fletcher::WithMetaRequired(
      *arrow::schema({arrow::field("voltage", voltage_type(), false)}), "output",
      ::fletcher::Mode::WRITE);
  return result;
}

auto BatteryParser::base_offset(size_t idx) -> size_t {
  // Hardware uses bit 19..12 to address one out of max 256 parser instances.
  // Hardware uses bit 11..0 to address registers within a parser.
  // Inidices here are 32-bit register indices, not byte addresses, so we divide by 4.
  return ((idx * 0x00001000) & 0x000FFFFF) / 4;
}

auto BatteryParser::custom_regs_offset(size_t idx) -> size_t {
  return base_offset(idx) + default_regs + range_regs_per_inst + in_addr_regs_per_inst +
         out_addr_regs_per_inst;
}

auto BatteryParser::ctrl_offset(size_t idx) -> size_t { return custom_regs_offset(idx); }

auto BatteryParser::status_offset(size_t idx) -> size_t {
  return custom_regs_offset(idx) + 1;
}

auto BatteryParser::result_rows_offset_lo(size_t idx) -> size_t {
  return custom_regs_offset(idx) + 2;
}

auto BatteryParser::result_rows_offset_hi(size_t idx) -> size_t {
  return custom_regs_offset(idx) + 3;
}

auto BatteryParser::input_firstidx_offset(size_t idx) -> size_t {
  return base_offset(idx) + default_regs;
}

auto BatteryParser::input_lastidx_offset(size_t idx) -> size_t {
  return input_firstidx_offset(idx) + 1;
}

auto BatteryParser::input_values_lo_offset(size_t idx) -> size_t {
  return base_offset(idx) + default_regs + range_regs_per_inst;
}

auto BatteryParser::input_values_hi_offset(size_t idx) -> size_t {
  return input_values_lo_offset(idx) + 1;
}

auto BatteryParser::output_voltage_offsets_lo_offset(size_t idx) -> size_t {
  return base_offset(idx) + default_regs + range_regs_per_inst + in_addr_regs_per_inst;
}

auto BatteryParser::output_voltage_offsets_hi_offset(size_t idx) -> size_t {
  return output_voltage_offsets_lo_offset(idx) + 1;
}

auto BatteryParser::output_voltage_values_lo_offset(size_t idx) -> size_t {
  return output_voltage_offsets_lo_offset(idx) + 2;
}

auto BatteryParser::output_voltage_values_hi_offset(size_t idx) -> size_t {
  return output_voltage_offsets_lo_offset(idx) + 3;
}

auto BatteryParser::Init() -> Status {
  if (idx_ >= 256) {
    return {Error::FletcherError,
            "Hardware does not allow more than 256 parser instances."};
  }
  // Write output addresses
  dau_t voltage_offsets, voltage_values;
  voltage_offsets.full = reinterpret_cast<da_t>(raw_out_offsets);
  voltage_values.full = reinterpret_cast<da_t>(raw_out_values);
  BOLSON_ROE(WriteMMIO(platform_, output_voltage_offsets_lo_offset(idx_),
                       voltage_offsets.lo, idx_, "output voltage offsets lo offset"));
  BOLSON_ROE(WriteMMIO(platform_, output_voltage_offsets_hi_offset(idx_),
                       voltage_offsets.hi, idx_, "output voltage offsets hi offset"));
  BOLSON_ROE(WriteMMIO(platform_, output_voltage_values_lo_offset(idx_),
                       voltage_values.lo, idx_, "output voltage values lo offset"));
  BOLSON_ROE(WriteMMIO(platform_, output_voltage_values_hi_offset(idx_),
                       voltage_values.hi, idx_, "output voltage values hi offset"));
  return Status::OK();
}

void AddBatteryOptionsToCLI(CLI::App* sub, BatteryOptions* out) {
  sub->add_option("--fpga-battery-num-parsers", out->num_parsers,
                  "Generic Fletcher \"battery status\" number of parser instances.")
      ->default_val(BOLSON_DEFAULT_FLETCHER_BATTERY_PARSERS);
  sub->add_flag("--fpga-battery-seq-col", out->seq_column,
                "Generic Fletcher \"battery status\" parser, retain ordering information "
                "by adding a sequence number column.")
      ->default_val(false);
}

}  // namespace bolson::parse::fpga
