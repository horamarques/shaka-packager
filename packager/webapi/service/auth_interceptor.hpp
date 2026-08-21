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

/// Collapses every run of '/' in |path| into a single '/'. Used so the
/// auth allowlist can't be bypassed by requests like "//api/v1/events":
/// oatpp's router treats slash runs as a single separator (Pattern.cpp
/// skipChar('/')) and still dispatches them, but a naive prefix check on
/// the raw path would miss them.
inline std::string CollapseSlashes(const std::string& path) {
  std::string result;
  result.reserve(path.size());
  for (char c : path) {
    if (c == '/' && !result.empty() && result.back() == '/')
      continue;
    result.push_back(c);
  }
  return result;
}

/// Constant-time comparison to avoid leaking the configured token's
/// contents through response-time timing side channels.
inline bool ConstantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size())
    return false;
  volatile unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff |= a[i] ^ b[i];
  return diff == 0;
}

/// Default-deny: every route requires 'Authorization: Bearer <token>' when
/// a token is configured, except an explicit allowlist (/health, /swagger*,
/// /api-docs*) evaluated on a slash-collapsed path so the check cannot be
/// bypassed with a request like "//api/v1/events".
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
    const std::string path =
        CollapseSlashes(request->getStartingLine().path.std_str());
    if (path == "/health" || path.rfind("/swagger", 0) == 0 ||
        path.rfind("/api-docs", 0) == 0) {
      return nullptr;  // open route
    }
    const auto header = request->getHeader("Authorization");
    if (header && ConstantTimeEquals(*header, "Bearer " + token_))
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
