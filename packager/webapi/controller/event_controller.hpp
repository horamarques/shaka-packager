// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>

#include <packager/metrics/metrics_service.h>
#include <packager/webapi/model/event_dto.hpp>
#include <packager/webapi/service/event_manager.h>
#include <packager/webapi/service/event_spec.h>

namespace shaka {
namespace webapi {

class EventController : public oatpp::web::server::api::ApiController {
 public:
  EventController(
      const std::shared_ptr<oatpp::parser::json::mapping::ObjectMapper>&
          object_mapper,
      std::shared_ptr<EventManager> event_manager)
      : oatpp::web::server::api::ApiController(object_mapper),
        event_manager_(std::move(event_manager)),
        requests_family_(
            &prometheus::BuildCounter()
                 .Name("shaka_api_requests_total")
                 .Help("API requests by route and status code.")
                 .Register(MetricsService::Instance().registry())),
        events_running_(
            &prometheus::BuildGauge()
                 .Name("shaka_api_events_running")
                 .Help("Events currently in STARTING or RUNNING state.")
                 .Register(MetricsService::Instance().registry())
                 .Add({})) {}

#include OATPP_CODEGEN_BEGIN(ApiController)

  ENDPOINT_INFO(createEvent) {
    info->summary = "Create and start a packaging event";
    info->addConsumes<Object<EventCreateDto>>("application/json");
    info->addResponse<Object<EventStatusDto>>(Status::CODE_201,
                                              "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_400, "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_409, "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_503, "application/json");
  }
  ENDPOINT("POST", "/api/v1/events", createEvent,
           BODY_DTO(Object<EventCreateDto>, body)) {
    EventCreateRequest request = FromDto(body);
    const std::string validation = ValidateEventRequest(request);
    if (!validation.empty()) {
      Count("create_event", Status::CODE_400.code);
      return Error(Status::CODE_400, "invalid_request", validation);
    }
    if (request.event_id.empty())
      request.event_id = GenerateEventId();

    const shaka::Status status = event_manager_->CreateEvent(
        request.event_id, BuildEventArgv(request),
        request.stop_timeout_seconds);
    if (!status.ok()) {
      const std::string message = status.error_message();
      if (message.find("duplicate") != std::string::npos) {
        Count("create_event", Status::CODE_409.code);
        return Error(Status::CODE_409, "duplicate_event", message);
      }
      if (message.find("exhausted") != std::string::npos) {
        Count("create_event", Status::CODE_503.code);
        return Error(Status::CODE_503, "resource_exhausted", message);
      }
      Count("create_event", Status::CODE_500.code);
      return Error(Status::CODE_500, "internal", message);
    }
    Count("create_event", Status::CODE_201.code);
    return createDtoResponse(Status::CODE_201,
                             ToDto(*event_manager_->GetEvent(request.event_id)));
  }

  ENDPOINT_INFO(listEvents) {
    info->summary = "List packaging events";
    info->addResponse<Object<EventListDto>>(Status::CODE_200,
                                            "application/json");
  }
  ENDPOINT("GET", "/api/v1/events", listEvents) {
    auto dto = EventListDto::createShared();
    dto->events = oatpp::List<oatpp::Object<EventStatusDto>>::createShared();
    for (const EventSnapshot& snap : event_manager_->ListEvents())
      dto->events->push_back(ToDto(snap));
    Count("list_events", Status::CODE_200.code);
    return createDtoResponse(Status::CODE_200, dto);
  }

  ENDPOINT_INFO(getEvent) {
    info->summary = "Get one event's status";
    info->addResponse<Object<EventStatusDto>>(Status::CODE_200,
                                              "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_404, "application/json");
  }
  ENDPOINT("GET", "/api/v1/events/{event_id}", getEvent,
           PATH(String, event_id)) {
    auto snap = event_manager_->GetEvent((*event_id));
    if (!snap.has_value()) {
      Count("get_event", Status::CODE_404.code);
      return Error(Status::CODE_404, "not_found", "unknown event");
    }
    Count("get_event", Status::CODE_200.code);
    return createDtoResponse(Status::CODE_200, ToDto(*snap));
  }

  ENDPOINT_INFO(stopEvent) {
    info->summary = "Stop an event (mode=drain default, mode=kill immediate)";
    info->addResponse<Object<EventStatusDto>>(Status::CODE_202,
                                              "application/json");
    info->addResponse<Object<ErrorDto>>(Status::CODE_404, "application/json");
  }
  ENDPOINT("DELETE", "/api/v1/events/{event_id}", stopEvent,
           PATH(String, event_id), QUERY(String, mode, "mode", "drain")) {
    const bool kill_now = (*mode) == "kill";
    const shaka::Status status =
        event_manager_->StopEvent((*event_id), kill_now);
    if (!status.ok()) {
      Count("stop_event", Status::CODE_404.code);
      return Error(Status::CODE_404, "not_found", status.error_message());
    }
    Count("stop_event", Status::CODE_202.code);
    return createDtoResponse(Status::CODE_202,
                             ToDto(*event_manager_->GetEvent(
                                 (*event_id))));
  }

  ENDPOINT_INFO(getLogs) {
    info->summary = "Tail of the event's stderr log";
  }
  ENDPOINT("GET", "/api/v1/events/{event_id}/logs", getLogs,
           PATH(String, event_id), QUERY(Int32, tail, "tail", 100)) {
    auto snap = event_manager_->GetEvent((*event_id));
    if (!snap.has_value()) {
      Count("get_logs", Status::CODE_404.code);
      return Error(Status::CODE_404, "not_found", "unknown event");
    }
    Count("get_logs", Status::CODE_200.code);
    return createResponse(Status::CODE_200,
                          TailFile(snap->log_path, *tail).c_str());
  }

  ENDPOINT_INFO(getEventMetrics) {
    info->summary = "Proxy one scrape of the event's Prometheus /metrics";
  }
  ENDPOINT("GET", "/api/v1/events/{event_id}/metrics", getEventMetrics,
           PATH(String, event_id)) {
    auto snap = event_manager_->GetEvent((*event_id));
    if (!snap.has_value()) {
      Count("get_event_metrics", Status::CODE_404.code);
      return Error(Status::CODE_404, "not_found", "unknown event");
    }
    auto body = HttpGetLocal(snap->metrics_port, "/metrics", 2000);
    if (!body.has_value()) {
      Count("get_event_metrics", Status::CODE_503.code);
      return Error(Status::CODE_503, "resource_exhausted",
                   "event metrics endpoint unreachable");
    }
    Count("get_event_metrics", Status::CODE_200.code);
    auto response = createResponse(Status::CODE_200, body->c_str());
    response->putHeader("Content-Type", "text/plain; version=0.0.4");
    return response;
  }

#include OATPP_CODEGEN_END(ApiController)

 private:
  std::shared_ptr<oatpp::web::protocol::http::outgoing::Response> Error(
      const oatpp::web::protocol::http::Status& status,
      const std::string& code, const std::string& message) {
    auto detail = ErrorDetailDto::createShared();
    detail->code = code.c_str();
    detail->message = message.c_str();
    auto error = ErrorDto::createShared();
    error->error = detail;
    return createDtoResponse(status, error);
  }

  static EventCreateRequest FromDto(const oatpp::Object<EventCreateDto>& dto);
  oatpp::Object<EventStatusDto> ToDto(const EventSnapshot& snap) {
    auto dto = EventStatusDto::createShared();
    dto->event_id = snap.id.c_str();
    dto->state = EventStateName(snap.state).c_str();
    dto->pid = snap.pid;
    if (snap.exit_code.has_value())
      dto->exit_code = snap.exit_code.value();
    dto->metrics_port = snap.metrics_port;
    dto->log_path = snap.log_path.c_str();
    dto->argv = oatpp::List<oatpp::String>::createShared();
    for (const std::string& arg : snap.argv)
      dto->argv->push_back(arg.c_str());
    dto->created_unix = snap.created_unix;
    dto->started_unix = snap.started_unix;
    dto->stopped_unix = snap.stopped_unix;
    dto->uptime_seconds =
        snap.started_unix == 0
            ? 0
            : (snap.stopped_unix != 0 ? snap.stopped_unix : time(nullptr)) -
                  snap.started_unix;
    return dto;
  }

  static std::string TailFile(const std::string& path, int lines);

  // Records one request against `shaka_api_requests_total{route,code}` and
  // refreshes `shaka_api_events_running` from the current event list.
  void Count(const char* route, int code) {
    requests_family_
        ->Add({{"route", route}, {"code", std::to_string(code)}})
        .Increment();
    int running = 0;
    for (const EventSnapshot& snap : event_manager_->ListEvents()) {
      if (snap.state == EventState::kStarting ||
          snap.state == EventState::kRunning)
        ++running;
    }
    events_running_->Set(running);
  }

  std::shared_ptr<EventManager> event_manager_;
  prometheus::Family<prometheus::Counter>* requests_family_;
  prometheus::Gauge* events_running_;
};

}  // namespace webapi
}  // namespace shaka
