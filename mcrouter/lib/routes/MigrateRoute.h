/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <folly/dynamic.h>
#include <folly/Optional.h>

#include "mcrouter/lib/config/RouteHandleFactory.h"
#include "mcrouter/lib/fbi/cpp/util.h"
#include "mcrouter/lib/fibers/FiberManager.h"
#include "mcrouter/lib/fibers/WhenN.h"
#include "mcrouter/lib/Operation.h"
#include "mcrouter/lib/OperationTraits.h"

namespace facebook { namespace memcache {

/**
 * This route handle changes behavior based on Migration mode.
 * 1. Before the migration starts, sends all requests to from_ route
 * handle.
 * 2. Between start time and (start_time + interval), sends all requests except
 * for deletes to from_ route handle and sends all delete requests to both from_
 * and to_ route handle. For delete requests, returns reply from
 * worst among two replies.
 * 3. Between (start time + interval) and (start_time + 2*interval), sends all
 * requests except for deletes to to_ route handle and sends all delete requests
 * to both from_ and to_ route handle. For delete requests, returns
 * reply from worst among two replies.
 * 4. After (start_time + 2*interval), sends all requests to to_ route handle.
 */
template <class RouteHandleIf, typename TimeProvider>
class MigrateRoute {
 public:
  static std::string routeName() { return "migrate"; }

  template <class Operation, class Request>
  std::vector<std::shared_ptr<RouteHandleIf>> couldRouteTo(
    const Request& req, Operation) const {

    auto mask = routeMask(req, Operation());
    std::vector<std::shared_ptr<RouteHandleIf>> ret;
    if (mask & kFromMask) {
      ret.push_back(from_);
    }
    if (mask & kToMask) {
      ret.push_back(to_);
    }
    return ret;
  }

  MigrateRoute(std::shared_ptr<RouteHandleIf> fh,
               std::shared_ptr<RouteHandleIf> th,
               time_t start_time_sec,
               time_t interval_sec,
               TimeProvider tp)
      : from_(std::move(fh)),
        to_(std::move(th)),
        startTimeSec_(start_time_sec),
        intervalSec_(interval_sec),
        tp_(tp) {

    assert(from_ != nullptr);
    assert(to_ != nullptr);
  }

  MigrateRoute(RouteHandleFactory<RouteHandleIf>& factory,
               const folly::dynamic& json,
               TimeProvider tp)
      : intervalSec_(3600),
        tp_(tp) {

    checkLogic(json.isObject(), "MigrateRoute should be object");
    checkLogic(json.count("start_time") && json["start_time"].isInt(),
               "MigrateRoute has no/invalid start_time");
    checkLogic(json.count("from"), "MigrateRoute has no 'from' route");
    checkLogic(json.count("to"), "MigrateRoute has no 'to' route");

    startTimeSec_ = json["start_time"].asInt();
    if (json.count("interval")) {
      checkLogic(json["interval"].isInt(),
                 "MigrateRoute interval is not integer");
      intervalSec_ = json["interval"].asInt();
    }

    from_ = factory.create(json["from"]);
    to_ = factory.create(json["to"]);

    assert(from_ != nullptr);
    assert(to_ != nullptr);
  }

  template <class Operation, class Request>
  typename ReplyType<Operation, Request>::type route(
    const Request& req, Operation) const {

    typedef typename ReplyType<Operation, Request>::type Reply;

    auto mask = routeMask(req, Operation());

    switch (mask) {
      case kFromMask: return from_->route(req, Operation());
      case kToMask: return to_->route(req, Operation());
      default: {
        auto& from = from_;
        auto& to = to_;
        std::function<Reply()> fs[2] {
          [&req, &from]() {
            return from->route(req, Operation());
          },
          [&req, &to]() {
            return to->route(req, Operation());
          }
        };

        folly::Optional<Reply> reply;
        fiber::forEach(fs, fs + 2,
          [&reply] (size_t id, Reply newReply) {
            if (!reply || newReply.worseThan(reply.value())) {
              reply = std::move(newReply);
            }
          });
        return std::move(reply.value());
      }
    }
  }
 private:
  static constexpr int kFromMask = 1;
  static constexpr int kToMask = 2;

  std::shared_ptr<RouteHandleIf> from_;
  std::shared_ptr<RouteHandleIf> to_;
  time_t startTimeSec_;
  time_t intervalSec_;
  const TimeProvider tp_;

  template <class Operation, class Request>
  int routeMask(
    const Request& req, Operation, typename DeleteLike<Operation>::Type = 0)
    const {

    const auto& creq = req;
    time_t curr = tp_(creq);

    if (curr < startTimeSec_) {
      return kFromMask;
    }

    if (curr < (startTimeSec_ + 2*intervalSec_)) {
      return kFromMask | kToMask;
    }

    /* else */
    return kToMask;
  }

  template <class Operation, class Request>
  int routeMask(
    const Request& req, Operation, OtherThanT(Operation, DeleteLike<>) = 0)
    const {

    const auto& creq = req;
    time_t curr = tp_(creq);

    if (curr < (startTimeSec_ + intervalSec_)) {
      return kFromMask;
    } else {
      return kToMask;
    }
  }
};

}}
