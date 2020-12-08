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

#include <memory>
#include <utility>

#include <arrow/api.h>
#include <arrow/json/api.h>
#include <illex/queue.h>
#include <blockingconcurrentqueue.h>

#include "bolson/convert/convert.h"
#include "bolson/status.h"
#include "bolson/stream.h"

namespace bolson::convert {

class ArrowIPCBuilder : public IPCBuilder {
 public:
  explicit ArrowIPCBuilder(arrow::json::ParseOptions parse_options,
                           const arrow::json::ReadOptions &read_options,
                           size_t json_threshold,
                           size_t batch_threshold,
                           size_t seq_buf_init_size = 1024 * 1024,
                           size_t str_buf_init_size = 1024 * 1024 * 16)
      : IPCBuilder(json_threshold,
                   batch_threshold,
                   seq_buf_init_size,
                   str_buf_init_size),
        parse_options(std::move(parse_options)), read_options(read_options) {}

  auto FlushBuffered(putong::Timer<> *t,
                     illex::LatencyTracker *lat_tracker) -> Status override;
 private:
  /// Arrow JSON parser parse options.
  arrow::json::ParseOptions parse_options;
  /// Arrow JSON parser read options.
  arrow::json::ReadOptions read_options;
};

/**
 * \brief Converts JSONs to Arrow RecordBatches, and batches to IPC messages.
 *
 * Multi-threaded.
 *
 * \param in                    The input queue of JSONs
 * \param out                   The output queue for Arrow IPC messages.
 * \param shutdown              Signal to shut down this thread (typically used when there
 *                              will be no more new inputs).
 * \param num_drones            Number of conversion threads to spawn.
 * \param json_buffer_threshold Threshold for the JSON buffer. When buffer grows over this
 *                              size, it will be converted to a batch.
 * \param batch_size_threshold  Threshold batch size. If batch goes over this size, it
 *                              will be converted to an IPC message and queued.
 * \param parse_options         The JSON parsing options for Arrow.
 * \param read_options          The JSON parsing read options for Arrow.
 * \param stats                 Statistics for each conversion thread.
 */
void ConvertWithCPU(illex::JSONQueue *in,
                    IpcQueue *out,
                    std::atomic<bool> *shutdown,
                    size_t num_drones,
                    const arrow::json::ParseOptions &parse_options,
                    const arrow::json::ReadOptions &read_options,
                    size_t json_buffer_threshold,
                    size_t batch_size_threshold,
                    illex::LatencyTracker *lat_tracker,
                    std::promise<std::vector<convert::Stats>> &&stats);

}
