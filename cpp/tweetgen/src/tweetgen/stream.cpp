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

#include <string>
#include <iostream>
#include <zmqpp/zmqpp.hpp>
#include <flitter/log.h>

#include "./stream.h"

namespace tweetgen {

auto StreamServer(const StreamOptions &opts) -> int {
  spdlog::info("Starting stream server.");

  const std::string endpoint = "tcp://*:" + std::to_string(opts.protocol.port);

  // Initialize the 0MQ context
  zmqpp::context context;
  // Generate a push socket
  zmqpp::socket_type type = zmqpp::socket_type::push;
  zmqpp::socket socket(context, type);
  // Bind to the socket
  spdlog::info("Binding to {}", endpoint);
  socket.bind(endpoint);

  // Receive the message
  spdlog::info("Producing {} messages.", opts.num_messages);

  for (size_t m = 0; m < opts.num_messages; m++) {
    // Generate a message with tweets in JSON format.
    auto json = GenerateTweets(opts.gen);
    StringBuffer buffer;
    rapidjson::Writer<StringBuffer> writer(buffer);
    json.Accept(writer);

    // Send the message.
    zmqpp::message message;
    message << buffer.GetString();
    socket.send(message);
  }

  // Send the end-of-stream marker.
  zmqpp::message stop;
  stop << opts.protocol.eos_marker;
  socket.send(stop);

  spdlog::info("Stream server shutting down.");

  return 0;
}

}  // namespace tweetgen
