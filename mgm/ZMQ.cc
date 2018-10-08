// ----------------------------------------------------------------------
// File: ZMQ.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#include "mgm/ZMQ.hh"
#include "mgm/fusex.pb.h"
#include "mgm/FuseServer.hh"
#include "common/Logging.hh"
#include <thread>

EOSMGMNAMESPACE_BEGIN

FuseServer ZMQ::gFuseServer;

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
ZMQ::ZMQ(const char* URL) : task(0), bindUrl(URL) {}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
ZMQ::ServeFuse()
{
  task = new Task(bindUrl);
  std::thread t1(&Task::run, task);
  t1.detach();
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
ZMQ::Task::run() noexcept
{
  int enable_ipv6 = 1;
#if ZMQ_VERSION >= ZMQ_MAKE_VERSION(4, 1, 0)
  frontend_.setsockopt(ZMQ_IPV6, &enable_ipv6, sizeof(enable_ipv6));
#else
  enable_ipv6 = 0;
  frontend_.setsockopt(ZMQ_IPV4ONLY, &enable_ipv6, sizeof(enable_ipv6));
#endif
  frontend_.bind(bindUrl.c_str());
  backend_.bind("inproc://backend");
  injector_.connect("inproc://backend");

  for (int i = 0; i < kMaxThread; ++i) {
    mWorkerThreads.push_back(new std::thread(&Worker::work,
                             new Worker(ctx_, ZMQ_DEALER)));
    mWorkerThreads.back()->detach();
  }

  try {
    zmq::proxy(static_cast<void*>(frontend_), static_cast<void*>(backend_),
               (void*)nullptr);
  } catch (const zmq::error_t& e) {
    if (e.num() == ETERM) {
      // Shutdown
      for (const auto th : mWorkerThreads) {
        delete th;
      }

      mWorkerThreads.clear();
      delete this;
    }
  }

  delete this;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
ZMQ::Worker::work()
{
  worker_.connect("inproc://backend");
  eos::fusex::container hb;

  try {
    while (true) {
      zmq::message_t identity;
      zmq::message_t msg;
      zmq::message_t copied_id;
      zmq::message_t copied_msg;
      worker_.recv(&identity);
      worker_.recv(&msg);
      std::string id((const char*) identity.data(), identity.size());
      std::string s((const char*) msg.data(), msg.size());
      hb.Clear();

      if (hb.ParseFromString(s)) {
        switch (hb.type()) {
        case hb.HEARTBEAT: {
          struct timespec tsnow;
          eos::common::Timing::GetTimeSpec(tsnow);
          hb.mutable_heartbeat_()->set_delta(tsnow.tv_sec - hb.heartbeat_().clock() +
                                             (((int64_t) tsnow.tv_nsec - (int64_t) hb.heartbeat_().clock_ns()) * 1.0 /
                                              1000000000.0));

          if (gFuseServer.Client().Dispatch(id, *(hb.mutable_heartbeat_()))) {
            if (EOS_LOGS_DEBUG)
              eos_static_debug("msg=\"received new heartbeat\" identity=%s type=%d",
                               (id.length() < 256) ? id.c_str() : "-illegal-", hb.type());
          } else {
            if (EOS_LOGS_DEBUG)
              eos_static_debug("msg=\"received heartbeat\" identity=%s type=%d",
                               (id.length() < 256) ? id.c_str() : "-illegal-", hb.type());
          }

          if (hb.statistics_().vsize_mb()) {
            gFuseServer.Client().HandleStatistics(id, hb.statistics_());
          }
        }
        break;

        case hb.MD: {
          gFuseServer.HandleMD(id, hb.md_());
        }
        break;

        case hb.DIR: {
          gFuseServer.HandleDir(id, hb.dir_());
        }
        break;

        default:
          eos_static_err("%s", "msg=\"message type unknown");
        }
      } else {
        eos_static_err("%s", "msg=\"unable to parse message\"");
      }
    }
  } catch (const zmq::error_t& e) {
    // Shutdown
    if (e.num() == ETERM) {
      eos_static_debug("%s", "msg=\"shutdown ZMQ worker ...\"");
      delete this;
    }
  }
}

EOSMGMNAMESPACE_END
