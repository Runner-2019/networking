/*
 * Copyright (c) 2023 Xiaoming Zhang
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

#include "tcp_connection.h"
#include "exec/linux/io_uring_context.hpp"
#include "exec/timed_scheduler.hpp"
#include "exec/variant_sender.hpp"
#include "http1/http_common.h"
#include "http1/http_error.h"
#include "http1/http_message_parser.h"
#include "http1/http_request.h"
#include "http1/http_response.h"
#include "sio/io_concepts.hpp"
#include "stdexec/execution.hpp"
#include "utils/if_then_else.h"
#include "utils/timeout.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <span>
#include <type_traits>
#include <utility>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

namespace net::tcp {

  using namespace stdexec; // NOLINT
  using namespace exec;    // NOLINT
  using namespace std;     // NOLINT

  // Get detailed error enum by message parser state.
  inline http1::Error detailed_error(http1::RequestParser::MessageState state) noexcept {
    using State = http1::RequestParser::MessageState;
    switch (state) {
    case State::kNothingYet:
      return http1::Error::kRecvRequestTimeoutWithNothing;
    case State::kStartLine:
    case State::kExpectingNewline:
      return http1::Error::kRecvRequestLineTimeout;
    case State::kHeader:
      return http1::Error::kRecvRequestHeadersTimeout;
    case State::kBody:
      return http1::Error::kRecvRequestBodyTimeout;
    case State::kCompleted:
      return http1::Error::kSuccess;
    }
  }

  inline bool check_parse_done(std::error_code ec) {
    return ec && ec != http1::Error::kNeedMore;
  }

  ex::sender auto parse_request(recv_state& state) {
    std::string ss = CopyArray(state.buffer); // DEBUG
    std::error_code ec{};                     // DEBUG
    std::size_t parsed_size = state.parser.Parse(ss, ec);
    if (ec == http1::Error::kNeedMore) {
      if (parsed_size < state.unparsed_size) {
        // WARN: This isn't efficient, a new way is needed.
        //       Move unparsed data to the front of buffer.
        state.unparsed_size -= parsed_size;
        ::memcpy(state.buffer.begin(), state.buffer.begin() + parsed_size, state.unparsed_size);
      }
    }
    return ex::just(check_parse_done(ec));
  }

  ex::sender auto finished(recv_state&& state) {
    return ex::if_then_else(
      state.parser.State() == http1::RequestParser::MessageState::kCompleted,
      ex::just(std::move(std::move(state).request), std::move(state).metric),
      ex::just_error(detailed_error(state.parser.State())));
  }

  void initialize_state(recv_state& state, const recv_option& opt) {
    if (opt.keepalive_timeout != unlimited_timeout) {
      state.remaining_time = opt.keepalive_timeout;
    } else {
      state.remaining_time = opt.total_timeout;
    }
  }

  void update_state(auto start, auto stop, std::size_t recv_size, recv_state& state) noexcept {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(start - stop);
    if (state.metric.time.first.time_since_epoch().count() == 0) {
      state.metric.time.first = start;
    }
    state.metric.time.last = stop;
    state.metric.time.elapsed += elapsed;
    state.metric.size.total += recv_size;
    state.unparsed_size += recv_size;
    state.remaining_time -= elapsed;
  }

  ex::sender auto check_recv_size(std::size_t recv_size) {
    return ex::if_then_else(
      recv_size != 0, ex::just(recv_size), ex::just_error(http1::Error::kEndOfStream));
  }

  // request, metrics
  ex::sender auto recv_request(const tcp_session& session) noexcept {
    using timepoint = std::chrono::time_point<std::chrono::system_clock>;
    return ex::just(recv_state{}) //
         | ex::let_value([&](recv_state& state) {
             auto recv_buffer = [&] {
               return std::span{state.buffer}.subspan(state.unparsed_size);
             };

             auto scheduler = session.socket.context_->get_scheduler();
             initialize_state(state, {});
             return sio::async::read_some(session.socket, recv_buffer())                       //
                  | ex::let_value(check_recv_size)                                             //
                  | ex::timeout(scheduler, state.remaining_time)                               //
                  | ex::let_stopped([] { return ex::just_error(http1::Error::kRecvTimeout); }) //
                  | ex::let_value([&](timepoint start, timepoint stop, std::size_t recv_size) {
                      update_state(start, stop, recv_size, state);
                      return ex::just();
                    })                                                  //
                  | ex::let_value([&] { return parse_request(state); }) //
                  | ex::repeat_effect_until()                           //
                  | ex::let_value([&] { return finished(std::move(state)); });
           });
  }

  ex::sender auto test_recv_request(const tcp_session& session) noexcept {
    using timepoint = std::chrono::time_point<std::chrono::system_clock>;
    ex::sender auto sndr =
      ex::just(recv_state{}) //
      | ex::let_value([&](recv_state& state) {
          auto recv_buffer = [&] {
            return std::span{state.buffer}.subspan(state.unparsed_size);
          };

          auto scheduler = session.socket.context_->get_scheduler();
          initialize_state(state, {});
          return sio::async::read_some(session.socket, recv_buffer())                       //
               | ex::let_value(check_recv_size)                                             //
               | ex::timeout(scheduler, state.remaining_time)                               //
               | ex::let_stopped([] { return ex::just_error(http1::Error::kRecvTimeout); }) //
               | ex::let_value([&](timepoint start, timepoint stop, std::size_t recv_size) {
                   update_state(start, stop, recv_size, state);
                   return ex::just();
                 })                                                  //
               | ex::let_value([&] { return parse_request(state); }) //
               | ex::repeat_effect_until()                           //
               | ex::let_value([&] { return finished(std::move(state)); });
        });
    sync_wait(std::move(sndr));
    return sndr;
  }

  ex::sender auto handle_request(http1::Request& request) noexcept {
    http1::Response response;
    // make response
    response.status_code = http1::HttpStatusCode::kOK;
    response.version = std::move(request).version;
    return ex::just(response);
  }

  ex::sender auto create_response(SocketSendMeta& meta) {
    std::optional<std::string> response_str = meta.response.MakeResponseString();
    if (response_str.has_value()) {
      meta.start_line_and_headers = std::move(*response_str);
    }
    return ex::if_then_else(
      response_str.has_value(), ex::just(), ex::just_error(http1::Error::kInvalidResponse));
  }

  // Update metrics
  // Need templates constexpr to check whether users need metrics.
  auto update_data(
    std::size_t send_size,
    SocketSendMeta& meta,
    [[maybe_unused]] tcp_session& session) {
    meta.total_send_size += send_size;
    std::cout << "send bytes: " << meta.total_send_size << "\n";
  }

  bool check_keepalive(const http1::Request& request) noexcept {
    if (request.ContainsHeader(http1::kHttpHeaderConnection)) {
      return true;
    }
    if (request.Version() == http1::HttpVersion::kHttp11) {
      return true;
    }
    return false;
  }

  // http1::Response&& Server::UpdateSession(http1::Response&& response, TcpSession& connection) {
  //   // bool need_keep_alive = response.ContainsHeader("Connection")
  //   // && response.HeaderValue("Connection") == "keep-alive";
  //   // connection.SetKeepAlive(need_keep_alive);
  //   return std::move(response);
  // }

  tcp_session create_session(tcp_socket socket) {
    return tcp_session{socket};
  }

  void update_server_metrics(server_metric server, recv_metric r) {
  }

  void update_server_metrics(server_metric server, send_metric s) {
  }

  template <class E>
  auto handle_error(E&& e) {
    if constexpr (std::is_same_v<E, std::error_code>) {
      std::cout << "Error orrcurred: " << std::forward<E>(e).message() << "\n";
    } else if constexpr (std::is_same_v<E, std::exception_ptr>) {
      std::cout << "Error orrcurred: exception_ptr\n";
    } else {
      std::cout << "Unknown Error orrcurred\n";
    }
  }

  void update_session(tcp_session& session) noexcept {
  }

  void start_server(Server& server) noexcept {
    auto handles = sio::async::use_resources(
      [&](tcp_acceptor_handle acceptor) noexcept {
        return sio::async::accept(acceptor) //
             | sio::let_value_each([&](tcp_socket socket) {
                 return ex::just(create_session(socket))          //
                      | ex::let_value([&](tcp_session& session) { //
                          return recv_request(session)            //
                               | ex::let_value([](auto&&...) { return ex::just(); })
                               // | ex::let_value([&](http1::Request&& req, recv_metric metrics) {
                               //     session.request = std::move(req);
                               //     server.update_metric(metrics);
                               //     return ex::just();
                               //   }) //
                               | ex::upon_error([]<class E>(auto&&...) {});
                        });
               })
             | sio::ignore_all();
      },
      server.acceptor);
    // ex::sync_wait(exec::when_any(handles, server.context.run()));
    ex::sync_wait(std::move(handles));
  }


} // namespace net::tcp

/*
 * template<net::server Server>
 * net::http::start_server(Server& server)
 *     let_value_with(socket)
 *         return handle_accepetd(socket)
 *                | let_value(create_session(socket))
 *                    return prepare_recv()
 *                           | recv_request(socket)
 *                           | update_metrics()
 *                           | handle_request()
 *                           | send_response()
 *                           | update_metrics()
 *                           | check_keepalive()
 *                           | repeat_effect_until()
 *                           | update_session()
 *                           | handle_error();
 *
 *
 *  start_server(server)                           -> void
 *  handle_accepetd(socket)                        -> void
 *  create_session(socket)                         -> tcp::session
 *  prepare_recv(&session)                         -> void
 *  recv_request(socket, recv_option)              -> sender<request, recv_metrics>
 *  handle_request(&request)                       -> sender<response>
 *  send_response(response, socket, send_option)   -> sender<send_metrics>
 *  update_metrics(&metrics)                       -> void
 *
 *  session: 
 *
 *
 *
 */
