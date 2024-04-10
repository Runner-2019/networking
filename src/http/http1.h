/*
 * Copyright (c) 2024 Xiaoming Zhang
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "http/http_common.h"
#include "http/v1/http1_request.h"
#include "http/v1/http1_message_parser.h"
#include "http/v1/http1_server.h"

namespace net::http {
  using http1_client_request = http1::request<http_message_direction::receive_from_client>;
  using http1_server_request = http1::request<http_message_direction::send_to_server>;

  using http1_client_request_parser = http1::message_parser<http1_client_request>;
  using http1_server_request_parser = http1::message_parser<http1_server_request>;

} // namespace net::http