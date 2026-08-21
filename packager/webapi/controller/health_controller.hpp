// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include <packager/version/version.h>
#include <packager/webapi/model/event_dto.hpp>

namespace shaka {
namespace webapi {

class HealthController : public oatpp::web::server::api::ApiController {
 public:
  explicit HealthController(
      const std::shared_ptr<oatpp::parser::json::mapping::ObjectMapper>&
          object_mapper)
      : oatpp::web::server::api::ApiController(object_mapper) {}

#include OATPP_CODEGEN_BEGIN(ApiController)

  ENDPOINT_INFO(health) {
    info->summary = "Liveness and packager version";
    info->addResponse<Object<HealthDto>>(Status::CODE_200, "application/json");
  }
  ENDPOINT("GET", "/health", health) {
    auto dto = HealthDto::createShared();
    dto->packager_version = GetPackagerVersion();
    return createDtoResponse(Status::CODE_200, dto);
  }

#include OATPP_CODEGEN_END(ApiController)
};

}  // namespace webapi
}  // namespace shaka
