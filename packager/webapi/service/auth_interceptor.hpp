// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <string>

#include <oatpp/web/protocol/http/outgoing/BufferBody.hpp>
#include <oatpp/web/server/interceptor/RequestInterceptor.hpp>

namespace shaka {
namespace webapi {

/// Rejects /api/v1/* requests lacking 'Authorization: Bearer <token>' when
/// a token is configured. /health and /swagger stay open.
class AuthInterceptor
    : public oatpp::web::server::interceptor::RequestInterceptor {
 public:
  explicit AuthInterceptor(std::string token) : token_(std::move(token)) {}

  std::shared_ptr<OutgoingResponse> intercept(
      const std::shared_ptr<IncomingRequest>& request) override {
    if (token_.empty())
      return nullptr;  // auth disabled
    // 1.3.0: getStartingLine().path is a StringKeyLabel (MemoryLabel), which
    // has std_str() directly; going through oatpp::String::toString() first
    // would land on a type that lacks std_str() in this version.
    const std::string path = request->getStartingLine().path.std_str();
    if (path.rfind("/api/v1/", 0) != 0)
      return nullptr;  // open route
    const auto header = request->getHeader("Authorization");
    if (header && (*header) == "Bearer " + token_)
      return nullptr;
    auto response = OutgoingResponse::createShared(
        oatpp::web::protocol::http::Status::CODE_401,
        oatpp::web::protocol::http::outgoing::BufferBody::createShared(
            oatpp::String(
                "{\"error\":{\"code\":\"unauthorized\","
                "\"message\":\"missing or invalid bearer token\"}}")));
    response->putHeader("Content-Type", "application/json");
    return response;
  }

 private:
  const std::string token_;
};

}  // namespace webapi
}  // namespace shaka
