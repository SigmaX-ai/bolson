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

#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <arrow/api.h>
#include <rapidjson/document.h>

#include "bolson/status.h"

namespace bolson {

/**
 * \brief Returns the total size in memory of all (nested) buffers backing Arrow ArrayData.
 *
 * Returns int64_t because Arrow.
 * \param array_data The ArrayData to analyze.
 * \returns The total size of all (nested) buffer contents in bytes.
 */
auto GetArrayDataSize(const std::shared_ptr<arrow::ArrayData> &array_data) -> int64_t;

/**
 * \brief Return the total size in memory of the data in an Arrow RecordBatch. Does not include buffer padding.
 * \param batch The RecordBatch to analyze.
 * \return The total size in bytes.
 */
auto GetBatchSize(const std::shared_ptr<arrow::RecordBatch> &batch) -> int64_t;

/// \brief Write an Arrow RecordBatch into a file as an Arrow IPC message.
auto WriteIPCMessageBuffer(const std::shared_ptr<arrow::RecordBatch> &batch) -> arrow::Result<
    std::shared_ptr<arrow::Buffer>>;

/// \brief Report some gigabytes per second.
void ReportGBps(const std::string &text, size_t bytes, double s, bool succinct = false);

/**
 * \brief Read num_bytes from a file and buffer it in memory. Appends a C-style string terminator to please rapidjson.
 * \param[in]  file_name    The file to load.
 * \param[in]  num_bytes    The number of bytes to read into the buffer.
 * \param[out] dest         The destination buffer.
 * \return The buffer, will be size num_bytes + 1 to accommodate the terminator character.
 */
auto LoadFile(const std::string &file_name,
              size_t num_bytes,
              std::vector<char> *dest) -> Status;

/**
 * \brief Convert a RapidJSON parsing error to a more readible format.
 * \param doc The document that has a presumed error.
 * \param file_buffer The buffer from which the document was attempted to be parsed.
 */
auto ConvertParserError(const rapidjson::Document &doc,
                        const std::vector<char> &file_buffer) -> std::string;

/**
 * \brief Convert a vector of T to a vector with pointers to each T.
 * \tparam T    The type of the items in the vector.
 * \param vec   The vector.
 * \return A vector with pointers to the items of vec.
 */
template<typename T>
auto ToPointers(std::vector<T> &vec) -> std::vector<T *> {
  std::vector<T *> result;
  result.reserve(vec.size());
  for (size_t i = 0; i < vec.size(); i++) {
    result.push_back(&vec[i]);
  }
  return result;
}

}  // namespace bolson
