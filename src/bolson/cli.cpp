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

#include "bolson/cli.h"

#include <arrow/io/api.h>

#include <CLI/CLI.hpp>
#include <algorithm>

#include "bolson/convert/converter.h"
#include "bolson/parse/implementations.h"
#include "bolson/publish/publisher.h"
#include "bolson/status.h"

namespace bolson {

static void AddClientOptionsToCLI(CLI::App* sub, illex::ClientOptions* client) {
  sub->add_option("--host", client->host, "JSON source TCP server hostname.")
      ->default_val("localhost");
  sub->add_option("--port", client->port, "JSON source TCP server port.")
      ->default_val(ILLEX_DEFAULT_PORT);
}

static void AddBenchOptionsToCLI(CLI::App* bench, BenchOptions* out) {
  // 'bench client' subcommand.
  auto* bench_client =
      bench->add_subcommand("client", "Run TCP client interface microbenchmark.");
  AddClientOptionsToCLI(bench_client, &out->client);

  // 'bench convert' subcommand.
  auto* bench_conv =
      bench->add_subcommand("convert", "Run JSON to Arrow IPC convert microbenchmark.");
  AddConverterOptionsToCLI(bench_conv, &out->convert.converter);
  bench_conv
      ->add_option("--total-json-bytes", out->convert.approx_total_bytes_str,
                   "Approximate number of JSON bytes to generate in total. If this is "
                   "set to zero, fill up input buffers until they are full. Accepts "
                   "scaling factors <n>Ki, <n>Mi, etc..")
      ->default_val(0);
  bench_conv
      ->add_flag("--parse-only", out->convert.parse_only,
                 "Only parse, skip resizing and serialization.")
      ->default_val(false);
  bench_conv->add_option("--seed", out->convert.generate.seed, "Generation seed.")
      ->default_val(0);
  bench_conv->add_option(
      "--latency", out->convert.latency_file,
      "When set, record batch latency measurements and write to supplied file.");
  bench_conv->add_option("--metrics", out->convert.metrics_file,
                         "When set, write other metrics to supplied file.");
  bench_conv
      ->add_option("--repeats", out->convert.repeats,
                   "Number of time to repeat parsing the same input.")
      ->default_val(1);

  // 'bench queue' subcommand
  auto* bench_queue = bench->add_subcommand("queue", "Run queue microbenchmark.");
  bench_queue->add_option("m,-m,--num-items,", out->queue.num_items)->default_val(256);

  // 'bench pulsar' subcommand
  auto* bench_pulsar =
      bench->add_subcommand("pulsar", "Run Pulsar publishing microbenchmark.");
  AddPublishBenchToCLI(bench_pulsar, &out->pulsar);
}

auto AppOptions::FromArguments(int argc, char** argv, AppOptions* out) -> Status {
  CLI::App app{"bolson : A JSON to Arrow IPC converter and Pulsar publishing tool."};

  app.require_subcommand();
  app.get_formatter()->column_width(50);

  // 'stream' subcommand:
  auto* stream =
      app.add_subcommand("stream", "Produce Pulsar messages from a JSON TCP stream.");
  stream->add_option("--latency", out->stream.latency_file,
                     "Enable batch latency measurements and write to supplied file.");
  stream->add_option("--metrics", out->stream.metrics_file,
                     "Write metrics to supplied file.");
  AddConverterOptionsToCLI(stream, &out->stream.converter);
  AddPublishOptsToCLI(stream, &out->stream.pulsar);
  AddClientOptionsToCLI(stream, &out->stream.client);

  // 'bench' subcommand:
  auto* bench =
      app.add_subcommand("bench", "Run micro-benchmarks on isolated pipeline stages.")
          ->require_subcommand();
  AddBenchOptionsToCLI(bench, &out->bench);

  // Attempt to parse the CLI arguments.
  try {
    app.parse(argc, argv);
  } catch (CLI::CallForHelp& e) {
    // User wants to see help.
    std::cout << app.help() << std::endl;
    return Status::OK();
  } catch (CLI::Error& e) {
    // There is some CLI error.
    std::cerr << app.help() << std::endl;
    return Status(Error::CLIError, "CLI Error: " + e.get_name() + ":" + e.what());
  }

  if (stream->parsed()) {
    out->sub = SubCommand::STREAM;
    out->stream.converter.ParseInput();
  } else if (bench->parsed()) {
    out->sub = SubCommand::BENCH;
    if (bench->get_subcommand_ptr("client")->parsed()) {
      out->bench.bench = Bench::CLIENT;
    } else if (bench->get_subcommand_ptr("convert")->parsed()) {
      out->bench.bench = Bench::CONVERT;
      BOLSON_ROE(out->bench.convert.ParseInput());
    } else if (bench->get_subcommand_ptr("pulsar")->parsed()) {
      out->bench.bench = Bench::PULSAR;
    } else if (bench->get_subcommand_ptr("queue")->parsed()) {
      out->bench.bench = Bench::QUEUE;
    }
  }

  return Status::OK();
}

}  // namespace bolson
