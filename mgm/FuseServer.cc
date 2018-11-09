//------------------------------------------------------------------------------
// File: FuseServer.cc
// Author: Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2017 CERN/Switzerland                                  *
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

#include <string>
#include <cstdlib>

#include "mgm/FuseServer.hh"
#include "mgm/Acl.hh"
#include "mgm/Policy.hh"
#include "mgm/Quota.hh"
#include "mgm/Recycle.hh"
#include "namespace/interface/IView.hh"
#include "namespace/interface/ContainerIterators.hh"
#include "namespace/Prefetcher.hh"
#include <thread>
#include <regex.h>
#include "common/Logging.hh"
#include "XrdMgmOfs.hh"
#include "mgm/ZMQ.hh"
#include "mgm/Stat.hh"
#include "common/Path.hh"

EOSMGMNAMESPACE_BEGIN

#define D_OK 8     // delete
#define M_OK 16    // chmod
#define C_OK 32    // chown
#define SA_OK 64   // set xattr
#define U_OK 128   // can update
#define SU_OK 256  // set utime

const char* k_mdino = "sys.eos.mdino";
const char* k_nlink = "sys.eos.nlink";


const char* FuseServer::cident = "fxserver";

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
FuseServer::FuseServer()
{
  SetLogId(logId, "fxserver");
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
FuseServer::~FuseServer()
{
  shutdown();
  std::cerr << __FUNCTION__ << ":: end of destructor" << std::endl;
}

//------------------------------------------------------------------------------
// Start method
//------------------------------------------------------------------------------
void
FuseServer::start()
{
  eos_static_info("msg=\"starting fuse server\"");
  std::thread monitorthread(&FuseServer::Clients::MonitorHeartBeat,
                            &(this->mClients));
  monitorthread.detach();
  std::thread capthread(&FuseServer::MonitorCaps, this);
  capthread.detach();
}

//------------------------------------------------------------------------------
// Shutdown method
//------------------------------------------------------------------------------
void
FuseServer::shutdown()
{
  Clients().terminate();
  terminate();
}

//------------------------------------------------------------------------------
// Dump message contents as json string
//------------------------------------------------------------------------------
std::string
FuseServer::dump_message(const google::protobuf::Message& message)
{
  google::protobuf::util::JsonPrintOptions options;
  options.add_whitespace = true;
  options.always_print_primitive_fields = true;
  std::string jsonstring;
  google::protobuf::util::MessageToJsonString(message, &jsonstring, options);
  return jsonstring;
}

//------------------------------------------------------------------------------
// Monitor heart beat
//------------------------------------------------------------------------------
void
FuseServer::Clients::MonitorHeartBeat()
{
  eos_static_info("msg=\"starting fusex heart beat thread\"");

  while (true) {
    client_uuid_t evictmap;
    client_uuid_t evictversionmap;
    {
      eos::common::RWMutexWriteLock lLock(*this);
      struct timespec tsnow;
      eos::common::Timing::GetTimeSpec(tsnow);

      for (auto it = map().begin(); it != map().end(); ++it) {
        double last_heartbeat = tsnow.tv_sec - it->second.heartbeat().clock() + (((
                                  int64_t) tsnow.tv_nsec - (int64_t) it->second.heartbeat().clock_ns()) * 1.0 /
                                1000000000.0);

        if (it->second.heartbeat().shutdown()) {
          evictmap[it->second.heartbeat().uuid()] = it->first;
          it->second.set_state(Client::EVICTED);
          eos_static_info("client='%s' shutdown", it->first.c_str());
        } else {
          if (last_heartbeat > mHeartBeatWindow) {
            if (last_heartbeat > mHeartBeatOfflineWindow) {
              if (last_heartbeat > mHeartBeatRemoveWindow) {
                evictmap[it->second.heartbeat().uuid()] = it->first;
                it->second.set_state(Client::EVICTED);
              } else {
                // drop locks once
                if (it->second.state() != Client::OFFLINE) {
                  gOFS->zMQ->gFuseServer.Locks().dropLocks(it->second.heartbeat().uuid());
                }

                it->second.set_state(Client::OFFLINE);
              }
            } else {
              it->second.set_state(Client::VOLATILE);
            }
          } else {
            it->second.set_state(Client::ONLINE);
          }
        }

        if (it->second.heartbeat().protversion() < it->second.heartbeat().PROTOCOLV2) {
          // protocol version mismatch, evict this client
          evictversionmap[it->second.heartbeat().uuid()] = it->first;
          it->second.set_state(Client::EVICTED);
        }
      }

      // delete clients to be evicted
      for (auto it = evictmap.begin(); it != evictmap.end(); ++it) {
        mMap.erase(it->second);
        mUUIDView.erase(it->first);
      }
    }

    // delete client ot be evicted because of a version mismatch
    for (auto it = evictversionmap.begin(); it != evictversionmap.end(); ++it) {
      std::string versionerror =
        "Server supports PROTOCOLV3 and requires atleast PROTOCOLV2";
      std::string uuid = it->first;
      Evict(uuid, versionerror);
      mMap.erase(it->second);
      mUUIDView.erase(it->first);
    }

    gOFS->zMQ->gFuseServer.Flushs().expireFlush();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (should_terminate()) {
      break;
    }
  }

  return ;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
bool
FuseServer::Clients::Dispatch(const std::string identity,
                              eos::fusex::heartbeat& hb)
{
  gOFS->MgmStats.Add("Eosxd::int::Heartbeat", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::Heartbeat");
  bool rc = true;
  eos::common::RWMutexWriteLock lLock(*this);
  std::set<Caps::shared_cap> caps_to_revoke;

  if (this->map().count(identity)) {
    rc = false;
  }

  // if heartbeats are older than the offline window, we just ignore them to avoid client 'waving'
  struct timespec tsnow;
  eos::common::Timing::GetTimeSpec(tsnow);
  double heartbeat_delay = tsnow.tv_sec - hb.clock() + (((
                             int64_t) tsnow.tv_nsec - (int64_t) hb.clock_ns()) * 1.0 / 1000000000.0);

  if (heartbeat_delay > mHeartBeatOfflineWindow) {
    eos_static_warning("delayed heartbeat from client=%s - delay=%.02f - dropping heartbeat",
                       identity.c_str(), heartbeat_delay);
    return rc;
  }

  (this->map())[identity].heartbeat() = hb;
  (this->uuidview())[hb.uuid()] = identity;
  lLock.Release();
  {
    // apply lifetime extensions requested by the client
    auto map = hb.mutable_authextension();

    for (auto it = map->begin(); it != map->end(); ++it) {
      Caps::shared_cap cap = gOFS->zMQ->gFuseServer.Cap().Get(it->first);

      if (cap && cap->vtime()) {
        eos_static_info("cap-extension: authid=%s vtime:= %u => %u",
                        it->first.c_str(),
                        cap->vtime(), cap->vtime() + it->second);
        cap->set_vtime(cap->vtime() + it->second);
      }
    }
  }
  {
    // apply auth revocation requested by the client
    auto map = hb.mutable_authrevocation();

    for (auto it = map->begin(); it != map->end(); ++it) {
      Caps::shared_cap cap = gOFS->zMQ->gFuseServer.Cap().Get(it->first);

      if (cap) {
        caps_to_revoke.insert(cap);
        eos_static_debug("cap-revocation: authid=%s vtime:= %u",
                         it->first.c_str(),
                         cap->vtime());
      }
    }
  }

  if (rc) {
    // ask a client to drop all caps when we see him the first time because we might have lost our caps due to a restart/failover
    BroadcastDropAllCaps(identity, hb);
    // communicate our current heart-beat interval
    eos::fusex::config cfg;
    cfg.set_hbrate(mHeartBeatInterval);
    cfg.set_dentrymessaging(true);
    BroadcastConfig(identity, cfg);
  } else {
    // revoke LEASES by cap
    for (auto it = caps_to_revoke.begin(); it != caps_to_revoke.end(); ++it) {
      eos::common::RWMutexWriteLock lLock(gOFS->zMQ->gFuseServer.Cap());
      gOFS->zMQ->gFuseServer.Cap().Remove(*it);
    }
  }

  EXEC_TIMING_END("Eosxd::int::Heartbeat");
  return rc;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::MonitorCaps() noexcept
{
  eos_static_info("msg=\"starting fusex monitor caps thread\"");
  std::map<FuseServer::Caps::authid_t, time_t> outofquota;
  uint64_t noquota = std::numeric_limits<long>::max() / 2;
  size_t cnt = 0;

  while (1) {
    EXEC_TIMING_BEGIN("Eosxd::int::MonitorCaps");

    // expire caps
    do {
      if (Cap().expire()) {
        Cap().pop();
      } else {
        break;
      }
    } while (1);

    time_t now = time(NULL);

    if (!(cnt % Clients().QuotaCheckInterval())) {
      // check quota nodes every mQuotaCheckInterval iterations
      typedef struct quotainfo {
        quotainfo(uid_t _uid, gid_t _gid, uint64_t _qid) : uid(_uid), gid(_gid),
          qid(_qid) {}
        quotainfo() : uid(0), gid(0), qid(0) {}
        uid_t uid;
        gid_t gid;
        uint64_t qid;
        std::vector<std::string> authids;
        std::string id()
        {
          char sid[64];
          snprintf(sid, sizeof(sid), "%u:%u:%lu", uid, gid, qid);
          return sid;
        }
      } quotainfo_t;
      std::map<std::string, quotainfo_t> qmap;
      {
        eos::common::RWMutexReadLock lLock(Cap());

        if (EOS_LOGS_DEBUG) {
          eos_static_debug("looping over caps n=%d", Cap().GetCaps().size());
        }

        std::map<FuseServer::Caps::authid_t, FuseServer::Caps::shared_cap>& allcaps =
          Cap().GetCaps();

        for (auto it = allcaps.begin(); it != allcaps.end(); ++it) {
          if (EOS_LOGS_DEBUG) {
            eos_static_debug("cap q-node %lx", it->second->_quota().quota_inode());
          }

          // if we find a cap with 'noquota' contents, we just ignore this one
          if (it->second->_quota().inode_quota() == noquota) {
            continue;
          }

          if (it->second->_quota().quota_inode()) {
            quotainfo_t qi(it->second->uid(), it->second->gid(),
                           it->second->_quota().quota_inode());

            // skip if we did this already ...
            if (qmap.count(qi.id())) {
              qmap[qi.id()].authids.push_back(it->second->authid());
            } else {
              qmap[qi.id()] = qi;
              qmap[qi.id()].authids.push_back(it->second->authid());
            }
          }
        }
      }

      for (auto it = qmap.begin(); it != qmap.end(); ++it) {
        eos::IContainerMD::id_t qino_id = it->second.qid;

        if (EOS_LOGS_DEBUG) {
          eos_static_debug("checking qino=%d", qino_id);
        }

        long long avail_bytes = 0;
        long long avail_files = 0;

        if (!Quota::QuotaBySpace(qino_id, it->second.uid, it->second.gid,
                                 avail_files, avail_bytes)) {
          for (auto auit = it->second.authids.begin();
               auit != it->second.authids.end(); ++auit) {
            if (EOS_LOGS_DEBUG)
              eos_static_debug("checking qino=%d files=%ld bytes=%ld authid=%s",
                               qino_id, avail_files, avail_bytes, auit->c_str());

            if (((!avail_files || !avail_bytes) && (!outofquota.count(*auit))) ||
                // first time out of quota
                ((avail_files && avail_bytes) &&
                 (outofquota.count(*auit)))) { // first time back to quota
              // send the changed quota information via a cap update
              FuseServer::Caps::shared_cap cap;
              {
                eos::common::RWMutexReadLock lLock(Cap());

                if (Cap().GetCaps().count(*auit)) {
                  cap = Cap().GetCaps()[*auit];
                }
              }

              if (cap) {
                cap->mutable__quota()->set_inode_quota(avail_files);
                cap->mutable__quota()->set_volume_quota(avail_bytes);
                // send this cap (again)
                Cap().BroadcastCap(cap);
              }

              // mark to not send this again unless the quota status changes
              if (!avail_files || !avail_bytes) {
                outofquota[*auit] = now;
              } else {
                outofquota.erase(*auit);
              }
            }
          }
        }
      }

      // expire some old out of quota entries
      for (auto it = outofquota.begin(); it != outofquota.end();) {
        if (((it->second) + 3600) < now) {
          auto erase_it = it++;
          outofquota.erase(erase_it);
        } else {
          it++;
        }
      }
    }

    EXEC_TIMING_END("Eosxd::int::MonitorCaps");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (should_terminate()) {
      break;
    }

    cnt++;

    if (gOFS) {
      gOFS->MgmStats.Add("Eosxd::int::MonitorCaps", 0, 0, 1);
    }
  }

  return ;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Print(std::string& out, std::string options, bool monitoring)
{
  if ((options.find("l") != std::string::npos) ||
      !options.length()) {
    Client().Print(out, options, monitoring);
  }

  if (options.find("f") != std::string::npos) {
    std::string flushout;
    gOFS->zMQ->gFuseServer.Flushs().Print(flushout);
    out += flushout;
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Clients::Print(std::string& out, std::string options,
                           bool monitoring)
{
  struct timespec tsnow;
  eos::common::Timing::GetTimeSpec(tsnow);
  std::map<std::string, size_t> clientcaps;
  {
    eos::common::RWMutexReadLock lLock(gOFS->zMQ->gFuseServer.Cap());

    // count caps per client uuid
    for (auto it = gOFS->zMQ->gFuseServer.Cap().InodeCaps().begin();
         it != gOFS->zMQ->gFuseServer.Cap().InodeCaps().end(); ++it) {
      for (auto sit = it->second.begin(); sit != it->second.end(); ++sit) {
        if (gOFS->zMQ->gFuseServer.Cap().GetCaps().count(*sit)) {
          FuseServer::Caps::shared_cap cap = gOFS->zMQ->gFuseServer.Cap().GetCaps()[*sit];
          clientcaps[cap->clientuuid()]++;
        }
      }
    }
  }
  eos::common::RWMutexReadLock lLock(*this);

  for (auto it = this->map().begin(); it != this->map().end(); ++it) {
    char formatline[4096];

    if (!monitoring) {
      if (!options.length() || (options.find("l") != std::string::npos))
        snprintf(formatline, sizeof(formatline),
                 "client : %-8s %32s %-8s %-8s %s %.02f %.02f %36s caps=%lu fds=%u\n",
                 it->second.heartbeat().name().c_str(),
                 it->second.heartbeat().host().c_str(),
                 it->second.heartbeat().version().c_str(),
                 it->second.status[it->second.state()],
                 eos::common::Timing::utctime(it->second.heartbeat().starttime()).c_str(),
                 tsnow.tv_sec - it->second.heartbeat().clock() +
                 (((int64_t) tsnow.tv_nsec -
                   (int64_t) it->second.heartbeat().clock_ns()) * 1.0 / 1000000000.0),
                 it->second.heartbeat().delta() * 1000,
                 it->second.heartbeat().uuid().c_str(),
                 clientcaps[it->second.heartbeat().uuid()],
                 it->second.statistics().open_files()
                );

      out += formatline;

      if (options.find("l") != std::string::npos) {
        snprintf(formatline, sizeof(formatline),
                 "......   ino          : %ld\n"
                 "......   ino-to-del   : %ld\n"
                 "......   ino-backlog  : %ld\n"
                 "......   ino-ever     : %ld\n"
                 "......   ino-ever-del : %ld\n"
                 "......   threads      : %d\n"
                 "......   vsize        : %.03f GB\n"
                 "......   rsize        : %.03f GB\n"
                 "......   leasetime    : %u s\n"
                 "......   open-files   : %u\n",
                 it->second.statistics().inodes(),
                 it->second.statistics().inodes_todelete(),
                 it->second.statistics().inodes_backlog(),
                 it->second.statistics().inodes_ever(),
                 it->second.statistics().inodes_ever_deleted(),
                 it->second.statistics().threads(),
                 it->second.statistics().vsize_mb() / 1024.0,
                 it->second.statistics().rss_mb() / 1024.0,
                 it->second.heartbeat().leasetime() ? it->second.heartbeat().leasetime() : 300,
                 it->second.statistics().open_files()
                );
        out += formatline;
      }

      std::map<uint64_t, std::set < pid_t>> rlocks;
      std::map<uint64_t, std::set < pid_t>> wlocks;
      gOFS->zMQ->gFuseServer.Locks().lsLocks(it->second.heartbeat().uuid(), rlocks,
                                             wlocks);

      for (auto rit = rlocks.begin(); rit != rlocks.end(); ++rit) {
        if (rit->second.size()) {
          snprintf(formatline, sizeof(formatline), "      t:rlock i:%016lx p:",
                   rit->first);
          out += formatline;
          std::string pidlocks;

          for (auto pit = rit->second.begin(); pit != rit->second.end(); ++pit) {
            if (pidlocks.length()) {
              pidlocks += ",";
            }

            char spid[16];
            snprintf(spid, sizeof(spid), "%u", *pit);
            pidlocks += spid;
          }

          out += pidlocks;
          out += "\n";
        }
      }

      for (auto wit = wlocks.begin(); wit != wlocks.end(); ++wit) {
        if (wit->second.size()) {
          snprintf(formatline, sizeof(formatline), "      t:wlock i:%016lx p:",
                   wit->first);
          out += formatline;
          std::string pidlocks;

          for (auto pit = wit->second.begin(); pit != wit->second.end(); ++pit) {
            if (pidlocks.length()) {
              pidlocks += ",";
            }

            char spid[16];
            snprintf(spid, sizeof(spid), "%u", *pit);
            pidlocks += spid;
          }

          out += pidlocks;
          out += "\n";
        }
      }
    } else {
      snprintf(formatline, sizeof(formatline) - 1, "_");
    }
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
size_t
FuseServer::Clients::leasetime(const std::string& uuid)
{
  // requires a Client read lock
  size_t leasetime = 0;

  if (this->uuidview().count(uuid) &&
      this->map().count(this->uuidview()[uuid])) {
    leasetime = this->map()[this->uuidview()[uuid]].heartbeat().leasetime();
  }

  if (leasetime > (7 * 86400)) {
    // don't allow longer lease times as a week
    leasetime = 7 * 86400;
  }

  return leasetime;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Clients::Evict(std::string& uuid, std::string reason)
{
  // prepare eviction message
  eos::fusex::response rsp;
  rsp.set_type(rsp.EVICT);
  rsp.mutable_evict_()->set_reason(reason);
  std::string rspstream;
  rsp.SerializeToString(&rspstream);
  eos::common::RWMutexReadLock lLock(*this);

  if (!mUUIDView.count(uuid)) {
    return ENOENT;
  }

  std::string id = mUUIDView[uuid];
  eos_static_info("msg=\"evicting client\" uuid=%s name=%s",
                  uuid.c_str(), id.c_str());
  gOFS->zMQ->mTask->reply(id, rspstream);
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Clients::Dropcaps(const std::string& uuid, std::string& out)
{
  eos::common::RWMutexWriteLock lLock(gOFS->zMQ->gFuseServer.Cap());
  out += " dropping caps of '";
  out += uuid;
  out += "' : ";

  if (!mUUIDView.count(uuid)) {
    return ENOENT;
  }

  std::string id = mUUIDView[uuid];
  Caps::ino_set_t cleanup_authids;

  for (auto it = gOFS->zMQ->gFuseServer.Cap().InodeCaps().begin();
       it != gOFS->zMQ->gFuseServer.Cap().InodeCaps().end(); ++it) {
    std::set<FuseServer::Caps::shared_cap> cap2delete;

    for (auto sit = it->second.begin(); sit != it->second.end(); ++sit) {
      if (gOFS->zMQ->gFuseServer.Cap().HasCap(*sit)) {
        FuseServer::Caps::shared_cap cap = gOFS->zMQ->gFuseServer.Cap().GetCaps()[*sit];

        if (cap->clientuuid() == uuid) {
          cap2delete.insert(cap);
          out += "\n ";
          char ahex[20];
          snprintf(ahex, sizeof(ahex), "%016lx", (unsigned long) cap->id());
          std::string match = "";
          match += "# i:";
          match += ahex;
          match += " a:";
          match += cap->authid();
          out += match;
        }
      }
    }

    for (auto scap = cap2delete.begin(); scap != cap2delete.end(); ++scap) {
      gOFS->zMQ->gFuseServer.Client().ReleaseCAP((uint64_t)(*scap)->id(),
          (*scap)->clientuuid(),
          (*scap)->clientid());
      eos_static_info("erasing %llx %s %s", (*scap)->id(),
                      (*scap)->clientid().c_str(), (*scap)->authid().c_str());
      // erase cap by auth id
      gOFS->zMQ->gFuseServer.Cap().GetCaps().erase((*scap)->authid());
      // erase cap by inode
      gOFS->zMQ->gFuseServer.Cap().InodeCaps()[(*scap)->id()].erase((
            *scap)->authid());

      if (!gOFS->zMQ->gFuseServer.Cap().InodeCaps()[(*scap)->id()].size()) {
        cleanup_authids.insert((*scap)->id());
      }

      gOFS->zMQ->gFuseServer.Cap().ClientCaps()[(*scap)->clientid()].erase((
            *scap)->authid());

      if (!gOFS->zMQ->gFuseServer.Cap().ClientCaps()[(*scap)->clientid()].size()) {
        gOFS->zMQ->gFuseServer.Cap().ClientCaps().erase((*scap)->clientid());
      }

      gOFS->zMQ->gFuseServer.Cap().ClientCaps()[(*scap)->clientid()].insert((
            *scap)->authid());
    }
  }

  for (auto it = cleanup_authids.begin(); it != cleanup_authids.end(); ++it) {
    if (!gOFS->zMQ->gFuseServer.Cap().InodeCaps()[*it].size()) {
      gOFS->zMQ->gFuseServer.Cap().InodeCaps().erase(*it);
    }
  }

  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Clients::ReleaseCAP(uint64_t md_ino,
                                const std::string& uuid,
                                const std::string& clientid
                               )
{
  gOFS->MgmStats.Add("Eosxd::int::ReleaseCap", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::ReleaseCap");
  // prepare release cap message
  eos::fusex::response rsp;
  rsp.set_type(rsp.LEASE);
  rsp.mutable_lease_()->set_type(eos::fusex::lease::RELEASECAP);
  rsp.mutable_lease_()->set_md_ino(md_ino);
  rsp.mutable_lease_()->set_clientid(clientid);
  std::string rspstream;
  rsp.SerializeToString(&rspstream);
  eos::common::RWMutexReadLock lLock(*this);

  if (!mUUIDView.count(uuid)) {
    return ENOENT;
  }

  std::string id = mUUIDView[uuid];
  eos_static_info("msg=\"asking cap release\" uuid=%s clientid=%s id=%lx",
                  uuid.c_str(), clientid.c_str(), md_ino);
  gOFS->zMQ->mTask->reply(id, rspstream);
  EXEC_TIMING_END("Eosxd::int::ReleaseCap");
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Clients::DeleteEntry(uint64_t md_ino,
                                 const std::string& uuid,
                                 const std::string& clientid,
                                 const std::string& name
                                )
{
  gOFS->MgmStats.Add("Eosxd::int::DeleteEntry", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::DeleteEntry");
  // prepare release cap message
  eos::fusex::response rsp;
  rsp.set_type(rsp.DENTRY);
  rsp.mutable_dentry_()->set_type(eos::fusex::dentry::REMOVE);
  rsp.mutable_dentry_()->set_name(name);
  rsp.mutable_dentry_()->set_md_ino(md_ino);
  rsp.mutable_dentry_()->set_clientid(clientid);
  std::string rspstream;
  rsp.SerializeToString(&rspstream);
  eos::common::RWMutexReadLock lLock(*this);

  if (!mUUIDView.count(uuid)) {
    return ENOENT;
  }

  std::string id = mUUIDView[uuid];
  eos_static_info("msg=\"asking dentry deletion\" uuid=%s clientid=%s id=%lx name=%s",
                  uuid.c_str(), clientid.c_str(), md_ino, name.c_str());
  gOFS->zMQ->mTask->reply(id, rspstream);
  EXEC_TIMING_END("Eosxd::int::DeleteEntry");
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Clients::SendMD(const eos::fusex::md& md,
                            const std::string& uuid,
                            const std::string& clientid,
                            uint64_t md_ino,
                            uint64_t md_pino,
                            uint64_t clock,
                            struct timespec& p_mtime
                           )
/*----------------------------------------------------------------------------*/

{
  gOFS->MgmStats.Add("Eosxd::int::SendMD", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::SendMD");
  // prepare update message
  eos::fusex::response rsp;
  rsp.set_type(rsp.MD);
  *(rsp.mutable_md_()) = md;
  rsp.mutable_md_()->set_type(eos::fusex::md::MD);
  // the client needs this to sort out the quota accounting using the cap map
  rsp.mutable_md_()->set_clientid(clientid);
  // when a file is created the inode is not yet written in the const md object
  rsp.mutable_md_()->set_md_ino(md_ino);
  rsp.mutable_md_()->set_md_pino(md_pino);

  if (p_mtime.tv_sec) {
    rsp.mutable_md_()->set_pt_mtime(p_mtime.tv_sec);
    rsp.mutable_md_()->set_pt_mtime_ns(p_mtime.tv_nsec);
  }

  rsp.mutable_md_()->set_clock(clock);
  std::string rspstream;
  rsp.SerializeToString(&rspstream);
  eos::common::RWMutexReadLock lLock(*this);

  if (!mUUIDView.count(uuid)) {
    return ENOENT;
  }

  std::string id = mUUIDView[uuid];
  eos_static_info("msg=\"sending md update\" uuid=%s clientid=%s id=%lx",
                  uuid.c_str(), clientid.c_str(), md.md_ino());
  gOFS->zMQ->mTask->reply(id, rspstream);
  EXEC_TIMING_END("Eosxd::int::SendMD");
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Clients::SendCAP(FuseServer::Caps::shared_cap cap)
/*----------------------------------------------------------------------------*/
{
  gOFS->MgmStats.Add("Eosxd::int::SendCAP", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::SendCAP");
  // prepare update message
  eos::fusex::response rsp;
  rsp.set_type(rsp.CAP);
  *(rsp.mutable_cap_()) = *cap;
  const std::string& uuid = cap->clientuuid();
  std::string rspstream;
  rsp.SerializeToString(&rspstream);
  eos::common::RWMutexReadLock lLock(*this);

  if (!mUUIDView.count(uuid)) {
    return ENOENT;
  }

  const std::string& clientid = mUUIDView[uuid];
  eos_static_info("msg=\"sending cap update\" uuid=%s clientid=%s cap-id=%lx",
                  uuid.c_str(), clientid.c_str(), cap->id());
  gOFS->zMQ->mTask->reply(clientid, rspstream);
  EXEC_TIMING_END("Eosxd::int::SendCAP");
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Clients::HandleStatistics(const std::string identity,
                                      const eos::fusex::statistics& stats)
{
  (this->map())[identity].statistics() = stats;

  if (EOS_LOGS_DEBUG) {
    eos_static_debug("");
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Caps::Store(const eos::fusex::cap& ecap,
                        eos::common::Mapping::VirtualIdentity* vid)
{
  gOFS->MgmStats.Add("Eosxd::int::Store", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::Store");
  eos::common::RWMutexWriteLock lLock(*this);
  eos_static_info("id=%lx clientid=%s authid=%s",
                  ecap.id(),
                  ecap.clientid().c_str(),
                  ecap.authid().c_str());

  // avoid to have multiple time entries for the same cap
  if (!mCaps.count(ecap.authid())) {
    // fill the three views on caps
    mTimeOrderedCap.insert(std::pair<time_t, authid_t>(ecap.vtime(),
                           ecap.authid()));
  }

  mClientCaps[ecap.clientid()].insert(ecap.authid());
  mClientInoCaps[ecap.clientid()].insert(ecap.id());
  shared_cap cap = std::make_shared<capx>();
  *cap = ecap;
  cap->set_vid(vid);
  mCaps[ecap.authid()] = cap;
  mInodeCaps[ecap.id()].insert(ecap.authid());
  EXEC_TIMING_END("Eosxd::int::Store");
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
bool
FuseServer::Caps::Imply(uint64_t md_ino,
                        FuseServer::Caps::authid_t authid,
                        FuseServer::Caps::authid_t implied_authid)
{
  eos_static_info("id=%lx authid=%s implied-authid=%s",
                  md_ino,
                  authid.c_str(),
                  implied_authid.c_str());
  shared_cap implied_cap = std::make_shared<capx>();
  shared_cap cap = Get(authid);

  if (!cap->id() || !implied_authid.length()) {
    return false;
  }

  *implied_cap = *cap;
  implied_cap->set_authid(implied_authid);
  implied_cap->set_id(md_ino);
  implied_cap->set_vid(cap->vid());
  struct timespec ts;
  eos::common::Timing::GetTimeSpec(ts, true);
  {
    size_t leasetime = 0;
    {
      eos::common::RWMutexReadLock lLock(gOFS->zMQ->gFuseServer.Client());
      leasetime = gOFS->zMQ->gFuseServer.Client().leasetime(cap->clientuuid());
    }
    eos::common::RWMutexWriteLock lock(*this);
    implied_cap->set_vtime(ts.tv_sec + (leasetime ? leasetime : 300));
    implied_cap->set_vtime_ns(ts.tv_nsec);
    // fill the three views on caps
    mTimeOrderedCap.insert(std::pair<time_t, authid_t>(implied_cap->vtime(),
                           implied_authid));
    mClientCaps[cap->clientid()].insert(implied_authid);
    mClientInoCaps[cap->clientid()].insert(md_ino);
    mCaps[implied_authid] = implied_cap;
    mInodeCaps[md_ino].insert(implied_authid);
  }
  return true;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
FuseServer::Caps::shared_cap
FuseServer::Caps::Get(FuseServer::Caps::authid_t id)
{
  eos::common::RWMutexWriteLock lLock(*this);

  if (mCaps.count(id)) {
    return mCaps[id];
  } else {
    return std::make_shared<capx>();
  }
}


/*----------------------------------------------------------------------------*/
int
FuseServer::Clients::SetHeartbeatInterval(int interval)
{
  // broadcast to all clients
  eos::common::RWMutexWriteLock lLock(*this);
  mHeartBeatInterval = interval;

  for (auto it = this->map().begin(); it != this->map().end(); ++it) {
    std::string uuid = it->second.heartbeat().uuid();
    std::string id = mUUIDView[uuid];

    if (id.length()) {
      eos::fusex::config cfg;
      cfg.set_hbrate(interval);
      cfg.set_dentrymessaging(true);
      BroadcastConfig(id, cfg);
    }
  }

  return 0;
}

/*----------------------------------------------------------------------------*/
int
FuseServer::Clients::SetQuotaCheckInterval(int interval)
{
  eos::common::RWMutexWriteLock lLock(*this);
  mQuotaCheckInterval = interval;
  return 0;
}

/*----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------*/
int
FuseServer::Clients::BroadcastConfig(const std::string& identity,
                                     eos::fusex::config& cfg)
/*----------------------------------------------------------------------------*/
{
  gOFS->MgmStats.Add("Eosxd::int::BcConfig", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::BcConfig");
  // prepare new heartbeat interval message
  eos::fusex::response rsp;
  rsp.set_type(rsp.CONFIG);
  *(rsp.mutable_config_()) = cfg;
  std::string rspstream;
  rsp.SerializeToString(&rspstream);
  eos_static_info("msg=\"broadcast config to client\" name=%s heartbeat-rate=%d",
                  identity.c_str(), cfg.hbrate());
  gOFS->zMQ->mTask->reply(identity, rspstream);
  EXEC_TIMING_END("Eosxd::int::BcConfig");
  return 0;
}

/*----------------------------------------------------------------------------*/
int
FuseServer::Clients::BroadcastDropAllCaps(const std::string& identity,
    eos::fusex::heartbeat& hb)
/*----------------------------------------------------------------------------*/
{
  gOFS->MgmStats.Add("Eosxd::int::BcDropAll", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::BcDropAll");
  // prepare drop all caps message
  eos::fusex::response rsp;
  rsp.set_type(rsp.DROPCAPS);
  std::string rspstream;
  rsp.SerializeToString(&rspstream);
  eos_static_info("msg=\"broadcast drop-all-caps to  client\" uuid=%s name=%s",
                  hb.uuid().c_str(), identity.c_str());
  gOFS->zMQ->mTask->reply(identity, rspstream);
  EXEC_TIMING_END("Eosxd::int::BcDropAll");
  return 0;
}


/*----------------------------------------------------------------------------*/
int
FuseServer::Caps::BroadcastReleaseFromExternal(uint64_t id)
/*----------------------------------------------------------------------------*/
{
  gOFS->MgmStats.Add("Eosxd::int::BcReleaseExt", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::BcReleaseExt");
  // broad-cast release for a given inode
  eos::common::RWMutexReadLock lLock(*this);
  eos_static_info("id=%lx ",
                  id);
  std::vector<shared_cap> bccaps;

  if (mInodeCaps.count(id)) {
    for (auto it = mInodeCaps[id].begin();
         it != mInodeCaps[id].end(); ++it) {
      shared_cap cap;

      // loop over all caps for that inode
      if (mCaps.count(*it)) {
        cap = mCaps[*it];
      } else {
        continue;
      }

      if (cap->id()) {
        bccaps.push_back(cap);
      }
    }
  }

  lLock.Release();

  for (auto it : bccaps) {
    gOFS->zMQ->gFuseServer.Client().ReleaseCAP((uint64_t) it->id(),
        it->clientuuid(),
        it->clientid());
    errno = 0 ; // seems that ZMQ function might set errno
  }

  EXEC_TIMING_END("Eosxd::int::BcReleaseExt");
  return 0;
}

int
FuseServer::Caps::BroadcastRelease(const eos::fusex::md& md)
{
  gOFS->MgmStats.Add("Eosxd::int::BcRelease", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::BcRelease");
  FuseServer::Caps::shared_cap refcap = Get(md.authid());
  eos::common::RWMutexReadLock lLock(*this);
  eos_static_info("id=%lx/%lx clientid=%s clientuuid=%s authid=%s",
                  refcap->id(),
                  md.md_pino(),
                  refcap->clientid().c_str(),
                  refcap->clientuuid().c_str(),
                  refcap->authid().c_str());
  std::vector<shared_cap> bccaps;
  uint64_t md_pino = refcap->id();

  if (!md_pino) {
    md_pino = md.md_pino();
  }

  if (mInodeCaps.count(md_pino)) {
    for (auto it = mInodeCaps[md_pino].begin();
         it != mInodeCaps[md_pino].end(); ++it) {
      shared_cap cap;

      // loop over all caps for that inode
      if (mCaps.count(*it)) {
        cap = mCaps[*it];
      } else {
        continue;
      }

      // skip our own cap!
      if (cap->authid() == md.authid()) {
        continue;
      }

      // skip identical client mounts!
      if (cap->clientuuid() == refcap->clientuuid()) {
        continue;
      }

      if (cap->id()) {
        bccaps.push_back(cap);
      }
    }
  }

  lLock.Release();

  for (auto it : bccaps) {
    gOFS->zMQ->gFuseServer.Client().ReleaseCAP((uint64_t) it->id(),
        it->clientuuid(),
        it->clientid());
    errno = 0 ;
  }

  EXEC_TIMING_END("Eosxd::int::BcRelease");
  return 0;
}

/*----------------------------------------------------------------------------*/
int
FuseServer::Caps::BroadcastDeletionFromExternal(uint64_t id,
    const std::string& name)
/*----------------------------------------------------------------------------*/
{
  gOFS->MgmStats.Add("Eosxd::int::BcDeletionExt", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::BcDeletionExt");
  // broad-cast deletion for a given name in a container
  eos::common::RWMutexReadLock lLock(*this);
  eos_static_info("id=%lx name=%s",
                  id,
                  name.c_str());
  std::vector<shared_cap> bccaps;

  if (mInodeCaps.count(id)) {
    for (auto it = mInodeCaps[id].begin();
         it != mInodeCaps[id].end(); ++it) {
      shared_cap cap;

      // loop over all caps for that inode
      if (mCaps.count(*it)) {
        cap = mCaps[*it];
      } else {
        continue;
      }

      if (cap->id()) {
        bccaps.push_back(cap);
      }
    }
  }

  lLock.Release();

  for (auto it : bccaps) {
    gOFS->zMQ->gFuseServer.Client().DeleteEntry((uint64_t) it->id(),
        it->clientuuid(),
        it->clientid(),
        name);
    errno = 0 ; // seems that ZMQ function might set errno
  }

  EXEC_TIMING_END("Eosxd::int::BcDeletionExt");
  return 0;
}

int
FuseServer::Caps::BroadcastDeletion(uint64_t id, const eos::fusex::md& md,
                                    const std::string& name)
{
  gOFS->MgmStats.Add("Eosxd::int::BcDeletion", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::BcDeletion");
  FuseServer::Caps::shared_cap refcap = Get(md.authid());
  eos::common::RWMutexReadLock lLock(*this);
  eos_static_info("id=%lx name=%s",
                  id,
                  name.c_str());
  std::vector<shared_cap> bccaps;

  if (mInodeCaps.count(refcap->id())) {
    for (auto it = mInodeCaps[refcap->id()].begin();
         it != mInodeCaps[refcap->id()].end(); ++it) {
      shared_cap cap;

      // loop over all caps for that inode
      if (mCaps.count(*it)) {
        cap = mCaps[*it];
      } else {
        continue;
      }

      // skip our own cap!
      if (cap->authid() == refcap->authid()) {
        continue;
      }

      // skip identical client mounts!
      if (cap->clientuuid() == refcap->clientuuid()) {
        continue;
      }

      if (cap->id()) {
        bccaps.push_back(cap);
      }
    }
  }

  lLock.Release();

  for (auto it : bccaps) {
    gOFS->zMQ->gFuseServer.Client().DeleteEntry((uint64_t) it->id(),
        it->clientuuid(),
        it->clientid(),
        name);
    errno = 0;
  }

  EXEC_TIMING_END("Eosxd::int::BcDeletion");
  return 0;
}

int
FuseServer::Caps::BroadcastCap(shared_cap cap)
{
  if (cap && cap->id()) {
    (void) gOFS->zMQ->gFuseServer.Client().SendCAP(cap);
  }

  return -1;
}

int
FuseServer::Caps::BroadcastMD(const eos::fusex::md& md,
                              uint64_t md_ino,
                              uint64_t md_pino,
                              uint64_t clock,
                              struct timespec& p_mtime)
{
  gOFS->MgmStats.Add("Eosxd::int::BcMD", 0, 0, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::BcMD");
  FuseServer::Caps::shared_cap refcap = Get(md.authid());
  eos::common::RWMutexReadLock lLock(*this);
  eos_static_info("id=%lx/%lx clientid=%s clientuuid=%s authid=%s",
                  refcap->id(),
                  md_pino,
                  refcap->clientid().c_str(),
                  refcap->clientuuid().c_str(),
                  refcap->authid().c_str());
  std::set<std::string> clients_sent;
  std::vector<shared_cap> bccaps;

  if (mInodeCaps.count(md_pino)) {
    for (auto it = mInodeCaps[md_pino].begin();
         it != mInodeCaps[md_pino].end(); ++it) {
      shared_cap cap;

      // loop over all caps for that inode
      if (mCaps.count(*it)) {
        cap = mCaps[*it];
      } else {
        continue;
      }

      eos_static_info("id=%lx clientid=%s clientuuid=%s authid=%s",
                      cap->id(),
                      cap->clientid().c_str(),
                      cap->clientuuid().c_str(),
                      cap->authid().c_str());

      // skip our own cap!
      if (cap->authid() == md.authid()) {
        continue;
      }

      // skip identical client mounts, the have it anyway!
      if (cap->clientuuid() == refcap->clientuuid()) {
        continue;
      }

      if (cap->id() && !clients_sent.count(cap->clientuuid())) {
        bccaps.push_back(cap);
        // make sure we sent the update only once to each client, eveh if this
        // one has many caps
        clients_sent.insert(cap->clientuuid());
      }
    }
  }

  lLock.Release();

  for (auto it : bccaps) {
    gOFS->zMQ->gFuseServer.Client().SendMD(md,
                                           it->clientuuid(),
                                           it->clientid(),
                                           md_ino,
                                           md_pino,
                                           clock,
                                           p_mtime);
    errno = 0;
  }

  EXEC_TIMING_END("Eosxd::int::BcMD");
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
std::string
FuseServer::Caps::Print(std::string option, std::string filter)
{
  std::string out;
  std::string astring;
  uint64_t now = (uint64_t) time(NULL);
  eos::common::RWMutexReadLock lock;

  if (option == "p") {
    lock.Grab(gOFS->eosViewRWMutex);
  }

  eos::common::RWMutexReadLock lLock(*this);
  eos_static_info("option=%s string=%s", option.c_str(), filter.c_str());
  regex_t regex;

  if (filter.size() &&
      regcomp(&regex, filter.c_str(), REG_ICASE | REG_EXTENDED | REG_NOSUB)) {
    out = "error: illegal regular expression ;";
    out += filter.c_str();
    out += "'\n";
    return out;
  }

  if (option == "t") {
    // print by time order
    for (auto it = mTimeOrderedCap.begin(); it != mTimeOrderedCap.end();) {
      if (!mCaps.count(it->second)) {
        it = mTimeOrderedCap.erase(it);
        continue;
      }

      char ahex[256];
      shared_cap cap = mCaps[it->second];
      snprintf(ahex, sizeof(ahex), "%016lx", (unsigned long) cap->id());
      std::string match = "";
      match += "# i:";
      match += ahex;
      match += " a:";
      match += cap->authid();
      match += " c:";
      match += cap->clientid();
      match += " u:";
      match += cap->clientuuid();
      match += " m:";
      snprintf(ahex, sizeof(ahex), "%08lx", (unsigned long) cap->mode());
      match += ahex;
      match += " v:";

      if ((cap->vtime() - now) >  0) {
        match += eos::common::StringConversion::GetSizeString(astring,
                 (unsigned long long) cap->vtime() - now);
      } else {
        match += eos::common::StringConversion::GetSizeString(astring,
                 (unsigned long long) 0);
      }

      match += "\n";

      if (filter.size() &&
          (regexec(&regex, match.c_str(), 0, NULL, 0) == REG_NOMATCH)) {
        it++;
        continue;
      }

      out += match.c_str();
      ++it;
    }
  }

  if (option == "i") {
    // print by inode
    for (auto it = mInodeCaps.begin(); it != mInodeCaps.end(); ++it) {
      char ahex[256];
      snprintf(ahex, sizeof(ahex), "%016lx", (unsigned long) it->first);

      if (filter.size() && (regexec(&regex, ahex, 0, NULL, 0) == REG_NOMATCH)) {
        continue;
      }

      out += "# i:";
      out += ahex;
      out += "\n";

      for (auto sit = it->second.begin(); sit != it->second.end(); ++sit) {
        out += "___ a:";
        out += *sit;

        if (!mCaps.count(*sit)) {
          out += " c:<unfound> u:<unfound> m:<unfound> v:<unfound>\n";
        } else {
          shared_cap cap = mCaps[*sit];
          out += " c:";
          out += cap->clientid();
          out += " u:";
          out += cap->clientuuid();
          out += " m:";
          snprintf(ahex, sizeof(ahex), "%016lx", (unsigned long) cap->mode());
          out += ahex;
          out += " v:";
          out += eos::common::StringConversion::GetSizeString(astring,
                 (unsigned long long) cap->vtime() - now);
          out += "\n";
        }
      }
    }
  }

  if (option == "p") {
    // print by inode
    for (auto it = mInodeCaps.begin(); it != mInodeCaps.end(); ++it) {
      std::string spath;

      try {
        if (eos::common::FileId::IsFileInode(it->first)) {
          std::shared_ptr<eos::IFileMD> fmd =
            gOFS->eosFileService->getFileMD(eos::common::FileId::InodeToFid(it->first));
          spath = "f:";
          spath += gOFS->eosView->getUri(fmd.get());
        } else {
          std::shared_ptr<eos::IContainerMD> cmd =
            gOFS->eosDirectoryService->getContainerMD(it->first);
          spath = "d:";
          spath += gOFS->eosView->getUri(cmd.get());
        }
      } catch (eos::MDException& e) {
        spath = "<unknown>";
      }

      if (filter.size() &&
          (regexec(&regex, spath.c_str(), 0, NULL, 0) == REG_NOMATCH)) {
        continue;
      }

      char apath[1024];
      out += "# ";
      snprintf(apath, sizeof(apath), "%-80s", spath.c_str());
      out += apath;
      out += "\n";

      for (auto sit = it->second.begin(); sit != it->second.end(); ++sit) {
        out += "___ a:";
        out += *sit;

        if (!mCaps.count(*sit)) {
          out += " c:<unfound> u:<unfound> m:<unfound> v:<unfound>\n";
        } else {
          shared_cap cap = mCaps[*sit];
          out += " c:";
          out += cap->clientid();
          out += " u:";
          out += cap->clientuuid();
          out += " m:";
          char ahex[20];
          snprintf(ahex, sizeof(ahex), "%016lx", (unsigned long) cap->mode());
          out += ahex;
          out += " v:";
          out += eos::common::StringConversion::GetSizeString(astring,
                 (unsigned long long) cap->vtime() - now);
          out += "\n";
        }
      }
    }
  }

  return out;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Caps::Delete(uint64_t md_ino)
{
  eos::common::RWMutexWriteLock lLock(*this);

  if (!mInodeCaps.count(md_ino)) {
    return ENOENT;
  }

  for (auto sit = mInodeCaps[md_ino].begin() ;
       sit != mInodeCaps[md_ino].end();
       ++sit) {
    for (auto it = mClientCaps.begin(); it != mClientCaps.end(); it++) {
      // erase authid from the client set
      it->second.erase(*sit);
    }

    if (mCaps.count(*sit)) {
      shared_cap cap = mCaps[*sit];

      if (mClientInoCaps.count(cap->clientid())) {
        mClientInoCaps[cap->clientid()].erase(md_ino);
      }

      mCaps.erase(*sit);
    }
  }

  // erase inode from the inode caps
  mInodeCaps.erase(md_ino);
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
FuseServer::Lock::shared_locktracker
FuseServer::Lock::getLocks(uint64_t id)
{
  XrdSysMutexHelper lock(this);

  // make sure you have this object locked
  if (!lockmap.count(id)) {
    lockmap[id] = std::make_shared<LockTracker>();
  }

  return lockmap[id];
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Lock::purgeLocks()
{
  XrdSysMutexHelper lock(this);
  std::set<uint64_t>purgeset;

  for (auto it = lockmap.begin(); it != lockmap.end(); ++it) {
    if (!it->second->inuse()) {
      purgeset.insert(it->first);
    }
  }

  for (auto it = purgeset.begin(); it != purgeset.end(); ++it) {
    lockmap.erase(*it);
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Lock::dropLocks(uint64_t id, pid_t pid)
{
  eos_static_info("id=%llu pid=%u", id, pid);
  // drop locks for a given inode/pid pair
  int retc = 0;
  {
    XrdSysMutexHelper lock(this);

    if (lockmap.count(id)) {
      lockmap[id]->removelk(pid);
      retc = 0;
    } else {
      retc = ENOENT;
    }
  }
  purgeLocks();
  return retc;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Lock::dropLocks(const std::string& owner)
{
  if (EOS_LOGS_DEBUG) {
    eos_static_debug("owner=%s", owner.c_str());
  }

  // drop locks for a given owner
  int retc = 0;
  {
    XrdSysMutexHelper lock(this);

    for (auto it = lockmap.begin(); it != lockmap.end(); ++it) {
      it->second->removelk(owner);
    }
  }
  purgeLocks();
  return retc;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::Lock::lsLocks(const std::string& owner,
                          std::map<uint64_t, std::set < pid_t >>& rlocks,
                          std::map<uint64_t, std::set < pid_t >>& wlocks)
{
  int retc = 0;
  {
    XrdSysMutexHelper lock(this);

    for (auto it = lockmap.begin(); it != lockmap.end(); ++it) {
      std::set<pid_t> rlk = it->second->getrlks(owner);
      std::set<pid_t> wlk = it->second->getwlks(owner);
      rlocks[it->first].insert(rlk.begin(), rlk.end());
      wlocks[it->first].insert(wlk.begin(), wlk.end());
    }
  }
  return retc;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Flush::beginFlush(uint64_t id, std::string client)
{
  eos_static_info("ino=%016x client=%s", id, client.c_str());
  XrdSysMutexHelper lock(this);
  flush_info_t finfo(client);
  flushmap[id][client].Add(finfo);
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Flush::endFlush(uint64_t id, std::string client)
{
  eos_static_info("ino=%016x client=%s", id, client.c_str());
  XrdSysMutexHelper lock(this);
  flush_info_t finfo(client);

  if (flushmap[id][client].Remove(finfo)) {
    flushmap[id].erase(client);

    if (!flushmap[id].size()) {
      flushmap.erase(id);
    }
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
bool
FuseServer::Flush::hasFlush(uint64_t id)
{
  // this function takes maximum 255ms and waits for a flush to be removed
  // this function might block a client connection/thread for the given time
  bool has = false;
  size_t delay = 1;

  for (size_t i = 0 ; i < 8; ++i) {
    {
      XrdSysMutexHelper lock(this);
      has = validateFlush(id);
    }

    if (!has) {
      return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    delay *= 2;
    ;
  }

  return true;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
bool
FuseServer::Flush::validateFlush(uint64_t id)
{
  bool has = false;

  if (flushmap.count(id)) {
    for (auto it = flushmap[id].begin(); it != flushmap[id].end();) {
      if (eos::common::Timing::GetAgeInNs(&it->second.ftime) < 0) {
        has = true;
        ++it;
      } else {
        it = flushmap[id].erase(it);
      }
    }

    if (!flushmap[id].size()) {
      flushmap.erase(id);
    }
  }

  return has;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Flush::expireFlush()
{
  XrdSysMutexHelper lock(this);

  for (auto it = flushmap.begin(); it != flushmap.end();) {
    for (auto fit = it->second.begin(); fit != it->second.end();) {
      if (eos::common::Timing::GetAgeInNs(&fit->second.ftime) < 0) {
        ++fit;
      } else {
        fit = it->second.erase(fit);
      }
    }

    if (!it->second.size()) {
      it = flushmap.erase(it);
    } else {
      ++it;
    }
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void
FuseServer::Flush::Print(std::string& out)
{
  XrdSysMutexHelper lock(this);

  for (auto it = flushmap.begin(); it != flushmap.end(); ++it) {
    for (auto fit = it->second.begin(); fit != it->second.end(); ++fit) {
      long long valid = eos::common::Timing::GetAgeInNs(&fit->second.ftime);
      char formatline[4096];
      snprintf(formatline, sizeof(formatline),
               "flush : ino : %016lx client : %-8s valid=%.02f sec\n",
               it->first,
               fit->first.c_str(),
               1.0 * valid / 1000000000.0);
      out += formatline;
    }
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
FuseServer::FillContainerMD(uint64_t id, eos::fusex::md& dir,
                            eos::common::Mapping::VirtualIdentity& vid)
{
  gOFS->MgmStats.Add("Eosxd::int::FillContainerMD", vid.uid, vid.gid, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::FillContainerMD");
  std::shared_ptr<eos::IContainerMD> cmd;
  eos::IContainerMD::ctime_t ctime;
  eos::IContainerMD::ctime_t mtime;
  uint64_t clock = 0;

  if (EOS_LOGS_DEBUG) {
    eos_debug("container-id=%llx", id);
  }

  try {
    cmd = gOFS->eosDirectoryService->getContainerMD(id, &clock);
    cmd->getCTime(ctime);
    cmd->getMTime(mtime);
    std::string fullpath = gOFS->eosView->getUri(cmd.get());
    dir.set_md_ino(id);
    dir.set_md_pino(cmd->getParentId());
    dir.set_ctime(ctime.tv_sec);
    dir.set_ctime_ns(ctime.tv_nsec);
    dir.set_mtime(mtime.tv_sec);
    dir.set_mtime_ns(mtime.tv_nsec);
    dir.set_atime(mtime.tv_sec);
    dir.set_atime_ns(mtime.tv_nsec);
    dir.set_size(cmd->getTreeSize());
    dir.set_uid(cmd->getCUid());
    dir.set_gid(cmd->getCGid());
    dir.set_mode(cmd->getMode());
    // @todo (apeters): no hardlinks
    dir.set_nlink(2);
    dir.set_name(cmd->getName());
    dir.set_fullpath(fullpath);
    eos::IFileMD::XAttrMap xattrs = cmd->getAttributes();

    for (const auto& elem : xattrs) {
      (*dir.mutable_attr())[elem.first] = elem.second;

      if ((elem.first) == "eos.btime") {
        std::string key, val;
        eos::common::StringConversion::SplitKeyValue(elem.second, key, val, ".");
        dir.set_btime(strtoul(key.c_str(), 0, 10));
        dir.set_btime_ns(strtoul(val.c_str(), 0, 10));
      }
    }

    dir.set_nchildren(cmd->getNumContainers() + cmd->getNumFiles());

    if (dir.operation() == dir.LS) {
      // we put a hard-coded listing limit for service protection
      if (dir.nchildren() > 32768) {
        // xrootd does not handle E2BIG ... sigh
        return ENAMETOOLONG;
      }

      for (auto it = eos::FileMapIterator(cmd); it.valid(); it.next()) {
        std::string key = eos::common::StringConversion::EncodeInvalidUTF8(it.key());
        (*dir.mutable_children())[key] =
          eos::common::FileId::FidToInode(it.value());
      }

      for (auto it = ContainerMapIterator(cmd); it.valid(); it.next()) {
        std::string key = eos::common::StringConversion::EncodeInvalidUTF8(it.key());
        (*dir.mutable_children())[key] = it.value();
      }

      // indicate that this MD record contains children information
      dir.set_type(dir.MDLS);
    } else {
      // indicate that this MD record contains only MD but no children information
      if (EOS_LOGS_DEBUG) {
        eos_debug("setting md type");
      }

      dir.set_type(dir.MD);
    }

    dir.set_clock(clock);
    dir.clear_err();
    EXEC_TIMING_END("Eosxd::int::FillContainerMD");
    return 0;
  } catch (eos::MDException& e) {
    errno = e.getErrno();
    eos_err("caught exception %d %s\n", e.getErrno(),
            e.getMessage().str().c_str());
    dir.set_err(errno);
    return errno;
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
bool
FuseServer::FillFileMD(uint64_t inode, eos::fusex::md& file,
                       eos::common::Mapping::VirtualIdentity& vid)
{
  gOFS->MgmStats.Add("Eosxd::int::FillFileMD", vid.uid, vid.gid, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::FillFileMD");
  // fills file meta data by inode number
  std::shared_ptr<eos::IFileMD> fmd, gmd;
  eos::IFileMD::ctime_t ctime;
  eos::IFileMD::ctime_t mtime;
  uint64_t clock = 0;

  if (EOS_LOGS_DEBUG) eos_debug("file-inode=%llx file-id=%llx", inode,
                                  eos::common::FileId::InodeToFid(inode));

  try {
    bool has_mdino = false;
    fmd = gOFS->eosFileService->getFileMD(eos::common::FileId::InodeToFid(inode),
                                          &clock);
    eos_debug("clock=%llx", clock);
    file.set_name(fmd->getName());
    gmd = fmd;

    if (fmd->hasAttribute(k_mdino)) {
      has_mdino = true;
      uint64_t mdino = std::stoll(fmd->getAttribute(k_mdino));
      fmd = gOFS->eosFileService->getFileMD(eos::common::FileId::InodeToFid(mdino),
                                            &clock);
      eos_debug("hlnk switched from %s to file %s (%#llx)",
                gmd->getName().c_str(), fmd->getName().c_str(), mdino);
    }

    /* fmd = link target file, gmd = link file */
    fmd->getCTime(ctime);
    fmd->getMTime(mtime);
    file.set_md_ino(eos::common::FileId::FidToInode(gmd->getId()));
    file.set_md_pino(fmd->getContainerId());
    file.set_ctime(ctime.tv_sec);
    file.set_ctime_ns(ctime.tv_nsec);
    file.set_mtime(mtime.tv_sec);
    file.set_mtime_ns(mtime.tv_nsec);
    file.set_btime(ctime.tv_sec);
    file.set_btime_ns(ctime.tv_nsec);
    file.set_atime(mtime.tv_sec);
    file.set_atime_ns(mtime.tv_nsec);
    file.set_size(fmd->getSize());
    file.set_uid(fmd->getCUid());
    file.set_gid(fmd->getCGid());

    if (fmd->isLink()) {
      file.set_mode(fmd->getFlags() | S_IFLNK);
      file.set_target(fmd->getLink());
    } else {
      file.set_mode(fmd->getFlags() | S_IFREG);
    }

    /* hardlinks */
    int nlink = 1;

    if (fmd->hasAttribute(k_nlink)) {
      nlink = std::stoi(fmd->getAttribute(k_nlink)) + 1;

      if (EOS_LOGS_DEBUG) {
        eos_debug("hlnk %s (%#lx) nlink %d", file.name().c_str(), fmd->getId(),
                  nlink);
      }
    }

    file.set_nlink(nlink);
    file.set_clock(clock);
    eos::IFileMD::XAttrMap xattrs = fmd->getAttributes();

    for (const auto& elem : xattrs) {
      if (has_mdino && ((elem.first) == k_nlink)) {
        continue;
      }

      (*file.mutable_attr())[elem.first] = elem.second;

      if ((elem.first) == "sys.eos.btime") {
        std::string key, val;
        eos::common::StringConversion::SplitKeyValue(elem.second, key, val, ".");
        file.set_btime(strtoul(key.c_str(), 0, 10));
        file.set_btime_ns(strtoul(val.c_str(), 0, 10));
      }
    }

    if (has_mdino) {
      (*file.mutable_attr())[k_mdino] = gmd->getAttribute(k_mdino);
    }

    file.clear_err();
    EXEC_TIMING_END("Eosxd::int::FillFileMD");
    return true;
  } catch (eos::MDException& e) {
    errno = e.getErrno();
    eos_err("caught exception %d %s\n", e.getErrno(),
            e.getMessage().str().c_str());
    file.set_err(errno);
    return false;
  }
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
bool
FuseServer::FillContainerCAP(uint64_t id,
                             eos::fusex::md& dir,
                             eos::common::Mapping::VirtualIdentity& vid,
                             std::string reuse_uuid,
                             bool issue_only_one)
{
  gOFS->MgmStats.Add("Eosxd::int::FillContainerCAP", vid.uid, vid.gid, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::FillContainerCAP");

  if (issue_only_one) {
    if (EOS_LOGS_DEBUG) {
      eos_debug("checking for id=%s", dir.clientid().c_str());
    }

    // check if the client has already a cap, in case yes, we don't return a new
    // one
    eos::common::RWMutexReadLock lLock(Cap());

    if (Cap().ClientInoCaps().count(dir.clientid())) {
      if (Cap().ClientInoCaps()[dir.clientid()].count(id)) {
        return true;
      }
    }
  }

  dir.mutable_capability()->set_id(id);

  if (EOS_LOGS_DEBUG) {
    eos_debug("container-id=%llx", id);
  }

  struct timespec ts;

  eos::common::Timing::GetTimeSpec(ts, true);

  size_t leasetime = 0;

  {
    eos::common::RWMutexReadLock lLock(gOFS->zMQ->gFuseServer.Client());
    leasetime = gOFS->zMQ->gFuseServer.Client().leasetime(dir.clientuuid());
    eos_debug("checking client %s leastime=%d", dir.clientid().c_str(),
              leasetime);
  }

  dir.mutable_capability()->set_vtime(ts.tv_sec + (leasetime ? leasetime : 300));
  dir.mutable_capability()->set_vtime_ns(ts.tv_nsec);
  std::string sysmask = (*(dir.mutable_attr()))["sys.mask"];
  long mask = 0777;

  if (sysmask.length()) {
    mask &= strtol(sysmask.c_str(), 0, 8);
  }

  mode_t mode = S_IFDIR;

  // define the permissions
  if (vid.uid == 0) {
    // grant all permissions
    dir.mutable_capability()->set_mode(0xff | S_IFDIR);
  } else  {
    if (vid.sudoer) {
      mode |= C_OK | M_OK | U_OK | W_OK | D_OK | SA_OK | SU_OK
              ; // chown + chmod permission + all the rest
    }

    if (vid.uid == (uid_t) dir.uid()) {
      // we don't apply a mask if we are the owner
      if (dir.mode() & S_IRUSR) {
        mode |= R_OK | M_OK | SU_OK;
      }

      if (dir.mode() & S_IWUSR) {
        mode |= U_OK | W_OK | D_OK | SA_OK | M_OK | SU_OK;
      }

      if (dir.mode() & mask & S_IXUSR) {
        mode |= X_OK;
      }
    }

    if (vid.gid == (gid_t) dir.gid()) {
      // we apply a mask if we are in the same group
      if (dir.mode() & mask & S_IRGRP) {
        mode |= R_OK;
      }

      if (dir.mode() & mask & S_IWGRP) {
        mode |= U_OK | W_OK | D_OK | SA_OK | M_OK | SU_OK;
      }

      if (dir.mode() & mask & S_IXGRP) {
        mode |= X_OK;
      }
    }

    // we apply a mask if we are matching other permissions
    if (dir.mode() & mask & S_IROTH) {
      mode |= R_OK;
    }

    if (dir.mode() & mask & S_IWOTH) {
      mode |= U_OK | W_OK | D_OK | SA_OK | M_OK | SU_OK;
    }

    if (dir.mode() & mask & S_IXOTH) {
      mode |= X_OK;
    }

    // look at ACLs
    std::string sysacl = (*(dir.mutable_attr()))["sys.acl"];
    std::string useracl = (*(dir.mutable_attr()))["user.acl"];

    if (sysacl.length() || useracl.length()) {
      bool evaluseracl = dir.attr().count("sys.eval.useracl") ? true : false;
      Acl acl;
      acl.Set(sysacl,
              useracl,
              vid,
              evaluseracl);

      if (EOS_LOGS_DEBUG)
        eos_debug("cap id=%lld evaluseracl %d CanRead %d CanWrite %d CanChmod %d CanChown %d CanUpdate %d CanNotDelete %d",
                  id, evaluseracl, acl.CanRead(), acl.CanWrite(), acl.CanChmod(), acl.CanChown(),
                  acl.CanUpdate(), acl.CanNotDelete());

      if (acl.IsMutable()) {
        if (acl.CanRead()) {
          mode |= R_OK;
        }

        if (acl.CanWrite() || acl.CanWriteOnce()) {
          mode |= W_OK | SA_OK | D_OK | M_OK;
        }

        if (acl.CanBrowse()) {
          mode |= X_OK;
        }

        if (acl.CanNotChmod()) {
          mode &= ~M_OK;
        }

        if (acl.CanChmod()) {
          mode |= M_OK;
        }

        if (acl.CanChown()) {
          mode |= C_OK;
        }

        if (acl.CanUpdate()) {
          mode |= U_OK | SA_OK;
        }

        // the owner can always delete
        if ((vid.uid != (uid_t) dir.uid()) && acl.CanNotDelete()) {
          mode &= ~D_OK;
        }
      }
    }

    dir.mutable_capability()->set_mode(mode);
  }

  std::string ownerauth = (*(dir.mutable_attr()))["sys.owner.auth"];

  // define new target owner
  if (ownerauth.length()) {
    if (ownerauth == "*") {
      // sticky ownership for everybody
      dir.mutable_capability()->set_uid(dir.uid());
      dir.mutable_capability()->set_gid(dir.gid());
    } else {
      ownerauth += ",";
      std::string ownerkey = vid.prot.c_str();
      std::string prot = vid.prot.c_str();
      ownerkey += ":";

      if (prot == "gsi") {
        ownerkey += vid.dn.c_str();
      } else {
        ownerkey += vid.uid_string.c_str();
      }

      if ((ownerauth.find(ownerkey)) != std::string::npos) {
        // sticky ownership for this authentication
        dir.mutable_capability()->set_uid(dir.uid());
        dir.mutable_capability()->set_gid(dir.gid());
      } else {
        // no sticky ownership for this authentication
        dir.mutable_capability()->set_uid(vid.uid);
        dir.mutable_capability()->set_gid(vid.gid);
      }
    }
  } else {
    // no sticky ownership
    dir.mutable_capability()->set_uid(vid.uid);
    dir.mutable_capability()->set_gid(vid.gid);
  }

  dir.mutable_capability()->set_authid(reuse_uuid.length() ?
                                       reuse_uuid : eos::common::StringConversion::random_uuidstring());
  dir.mutable_capability()->set_clientid(dir.clientid());
  dir.mutable_capability()->set_clientuuid(dir.clientuuid());

  // max-filesize settings
  if (dir.attr().count("sys.forced.maxsize")) {
    // dynamic upper file size limit per file
    dir.mutable_capability()->set_max_file_size(strtoull((*
        (dir.mutable_attr()))["sys.forced.maxsize"].c_str(), 0, 10));
  } else {
    // hard-coded upper file size limit per file
    dir.mutable_capability()->set_max_file_size(512ll * 1024ll * 1024ll *
        1024ll); // 512 GB
  }

  std::string space = "default";
  {
    // add quota information
    if (dir.attr().count("sys.forced.space")) {
      space = (*(dir.mutable_attr()))["sys.forced.space"];
    } else {
      if (dir.attr().count("user.forced.space")) {
        space = (*(dir.mutable_attr()))["user.forced.space"];
      }
    }

    // Check if quota is enabled for the current space
    bool has_quota = false;
    long long avail_bytes = 0;
    long long avail_files = 0;
    eos::IContainerMD::id_t quota_inode = 0;

    if (eos::mgm::FsView::gFsView.IsQuotaEnabled(space)) {
      if (!Quota::QuotaByPath(dir.fullpath().c_str(), dir.capability().uid(),
                              dir.capability().gid(), avail_files, avail_bytes,
                              quota_inode)) {
        has_quota = true;
      }
    } else {
      avail_files = std::numeric_limits<long>::max() / 2;
      avail_bytes = std::numeric_limits<long>::max() / 2;
      has_quota = true;
    }

    dir.mutable_capability()->mutable__quota()->set_inode_quota(avail_files);
    dir.mutable_capability()->mutable__quota()->set_volume_quota(avail_bytes);
    dir.mutable_capability()->mutable__quota()->set_quota_inode(quota_inode);

    if (!has_quota) {
      dir.mutable_capability()->mutable__quota()->clear_inode_quota();
      dir.mutable_capability()->mutable__quota()->clear_volume_quota();
      dir.mutable_capability()->mutable__quota()->clear_quota_inode();
    }
  }
  EXEC_TIMING_END("Eosxd::int::FillContainerCAP");
  Cap().Store(dir.capability(), &vid);
  return true;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
FuseServer::Caps::shared_cap
FuseServer::ValidateCAP(const eos::fusex::md& md, mode_t mode,
                        eos::common::Mapping::VirtualIdentity& vid)
{
  errno = 0 ;
  FuseServer::Caps::shared_cap cap = Cap().Get(md.authid());

  // no cap - go away
  if (!cap->id()) {
    eos_static_err("no cap for authid=%s", md.authid().c_str());
    errno = ENOENT;
    return 0;
  }

  // wrong cap - go away
  if ((cap->id() != md.md_ino()) && (cap->id() != md.md_pino())) {
    eos_static_err("wrong cap for authid=%s cap-id=%lx md-ino=%lx md-pino=%lx",
                   md.authid().c_str(), md.md_ino(), md.md_pino());
    errno = EINVAL;
    return 0;
  }

  if (EOS_LOGS_DEBUG) {
    eos_static_debug("cap-mode=%x mode=%x", cap->mode(), mode);
  }

  if ((cap->mode() & mode) == mode) {
    uint64_t now = (uint64_t) time(NULL);

    // leave some margin for revoking
    if (cap->vtime() <= (now + 60)) {
      // cap expired !
      errno = ETIMEDOUT;
      return 0;
    }

    return cap;
  }

  errno = EPERM;
  return 0;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
uint64_t
FuseServer::InodeFromCAP(const eos::fusex::md& md)
{
  FuseServer::Caps::shared_cap cap = Cap().Get(md.authid());

  // no cap - go away
  if (!cap) {
    if (EOS_LOGS_DEBUG) {
      eos_static_debug("no cap for authid=%s", md.authid().c_str());
    }

    return 0;
  } else {
    if (EOS_LOGS_DEBUG) {
      eos_static_debug("authid=%s cap-ino=%lx", md.authid().c_str(), cap->id());
    }
  }

  return cap->id();
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
std::string
FuseServer::Header(const std::string& response)
{
  char hex[9];
  sprintf(hex, "%08x", (int) response.length());
  return std::string("[") + hex + std::string("]");
}

/*----------------------------------------------------------------------------*/
bool
FuseServer::ValidatePERM(const eos::fusex::md& md, const std::string& mode,
                         eos::common::Mapping::VirtualIdentity& vid,
                         bool take_lock)
{
  gOFS->MgmStats.Add("Eosxd::int::ValidatePERM", vid.uid, vid.gid, 1);
  EXEC_TIMING_BEGIN("Eosxd::int::ValidatePERM");
  // -------------------------------------------------------------------------------------------------------------
  // - when an MGM was restarted it does not know anymore any client CAPs, but we can fallback to validate
  //   permissions on the fly again
  // -------------------------------------------------------------------------------------------------------------
  eos_info("mode=%s", mode.c_str());
  std::string path;
  shared_ptr<eos::IContainerMD> cmd;
  uint64_t clock = 0;
  bool r_ok = false;
  bool w_ok = false;
  bool x_ok = false;
  bool d_ok = false;
  eos::common::RWMutexReadLock rd_ns_lock;

  if (take_lock) {
    rd_ns_lock.Grab(gOFS->eosViewRWMutex);
  }

  try {
    if (S_ISDIR(md.mode())) {
      cmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino(), &clock);
    } else {
      cmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino(), &clock);
    }

    path = gOFS->eosView->getUri(cmd.get());
    // for performance reasons we implement a seperate access control check here, because
    // we want to avoid another id=path translation and unlock lock of the namespace
    eos::IContainerMD::XAttrMap attrmap = cmd->getAttributes();

    if (cmd->access(vid.uid, vid.gid, R_OK)) {
      r_ok = true;
    }

    if (cmd->access(vid.uid, vid.gid, W_OK)) {
      w_ok = true;
      d_ok = true;
    }

    if (cmd->access(vid.uid, vid.gid, X_OK)) {
      x_ok = true;
    }

    // ACL and permission check
    Acl acl(attrmap, vid);
    eos_info("acl=%d r=%d w=%d wo=%d x=%d egroup=%d mutable=%d",
             acl.HasAcl(), acl.CanRead(), acl.CanWrite(), acl.CanWriteOnce(),
             acl.HasAcl(), acl.CanRead(), acl.CanWrite(), acl.CanWriteOnce(),
             acl.CanBrowse(), acl.HasEgroup(), acl.IsMutable());

    // browse permission by ACL
    if (acl.HasAcl()) {
      if (acl.CanWrite()) {
        w_ok = true;
        d_ok = true;
      }

      // write-once excludes updates
      if (!(acl.CanWrite() || acl.CanWriteOnce())) {
        w_ok = false;
      }

      // deletion might be overwritten/forbidden
      if (acl.CanNotDelete()) {
        d_ok = false;
      }

      // the r/x are added to the posix permissions already set
      if (acl.CanRead()) {
        r_ok |= true;
      }

      if (acl.CanBrowse()) {
        x_ok |= true;
      }

      if (!acl.IsMutable()) {
        w_ok = d_ok = false;
      }
    }
  } catch (eos::MDException& e) {
    eos_err("failed to get directory inode ino=%16x", md.md_pino());
    return false;
  }

  std::string accperm;
  accperm = "R";

  if (r_ok) {
    accperm += "R";
  }

  if (w_ok) {
    accperm += "WCKNV";
  }

  if (d_ok) {
    accperm += "D";
  }

  EXEC_TIMING_END("Eosxd::int::ValidatePERM");

  if (accperm.find(mode) != std::string::npos) {
    eos_info("allow access to ino=%16x request-mode=%s granted-mode=%s",
             md.md_pino(),
             mode.c_str(),
             accperm.c_str()
            );
    return true;
  } else {
    eos_err("reject access to ino=%16x request-mode=%s granted-mode=%s",
            md.md_pino(),
            mode.c_str(),
            accperm.c_str()
           );
    return false;
  }
}


/*----------------------------------------------------------------------------*/
int
FuseServer::HandleMD(const std::string& id,
                     const eos::fusex::md& md,
                     eos::common::Mapping::VirtualIdentity& vid,
                     std::string* response,
                     uint64_t* clock)
{
  std::string ops;
  int op_type = md.operation();

  if (op_type == md.GET) {
    ops = "GET";
  } else if (op_type == md.SET) {
    ops = "SET";
  } else if (op_type == md.DELETE) {
    ops = "DELETE";
  } else if (op_type == md.GETCAP) {
    ops = "GETCAP";
  } else if (op_type == md.LS) {
    ops = "LS";
  } else if (op_type == md.GETLK) {
    ops = "GETLK";
  } else if (op_type == md.SETLK) {
    ops = "SETLK";
  } else if (op_type == md.SETLKW) {
    ops = "SETLKW";
  } else if (op_type == md.BEGINFLUSH) {
    ops = "BEGINFLUSH";
  } else if (op_type == md.ENDFLUSH) {
    ops = "ENDFLUSH";
  } else {
    ops = "UNKOWN";
  }

  eos_info("ino=%016lx operation=%s cid=%s cuuid=%s", (long) md.md_ino(),
           ops.c_str(),
           md.clientid().c_str(), md.clientuuid().c_str());

  if (EOS_LOGS_DEBUG) {
    std::string mdout = dump_message(md);
    eos_debug("\n%s\n", mdout.c_str());
  }

  if (md.operation() == md.BEGINFLUSH) {
    gOFS->MgmStats.Add("Eosxd::ext::BEGINFLUSH", vid.uid, vid.gid, 1);
    EXEC_TIMING_BEGIN("Eosxd::ext::BEGINFLUSH");
    // this is a flush begin/end indicator
    Flushs().beginFlush(md.md_ino(), md.clientuuid());
    eos::fusex::response resp;
    resp.set_type(resp.NONE);
    resp.SerializeToString(response);
    EXEC_TIMING_END("Eosxd::ext::BEGINFLUSH");
    return 0;
  }

  if (md.operation() == md.ENDFLUSH) {
    gOFS->MgmStats.Add("Eosxd::ext::ENDFLUSH", vid.uid, vid.gid, 1);
    EXEC_TIMING_BEGIN("Eosxd::ext::ENDFLUSH");
    Flushs().endFlush(md.md_ino(), md.clientuuid());
    eos::fusex::response resp;
    resp.set_type(resp.NONE);
    resp.SerializeToString(response);
    EXEC_TIMING_END("Eosxd::ext::ENDFLUSH");
    return 0;
  }

  if ((md.operation() == md.GET) || (md.operation() == md.LS)) {
    if (clock) {
      *clock = 0 ;
    }

    eos::fusex::container cont;
    eos::common::RWMutexReadLock rd_fs_lock(eos::mgm::FsView::gFsView.ViewMutex);
    eos::common::RWMutexReadLock rd_ns_lock(gOFS->eosViewRWMutex);

    if (!eos::common::FileId::IsFileInode(md.md_ino())) {
      eos_info("ino=%lx get-dir", (long) md.md_ino());
      cont.set_type(cont.MDMAP);
      cont.set_ref_inode_(md.md_ino());
      eos::fusex::md_map* mdmap = cont.mutable_md_map_();
      // create the parent entry;
      auto parent = mdmap->mutable_md_map_();
      (*parent)[md.md_ino()].set_md_ino(md.md_ino());
      (*parent)[md.md_ino()].set_clientuuid(md.clientuuid());
      (*parent)[md.md_ino()].set_clientid(md.clientid());
      EXEC_TIMING_BEGIN((md.operation() == md.LS) ? "Eosxd::ext::LS" :
                        "Eosxd::ext::GET");

      if (md.operation() == md.LS) {
        gOFS->MgmStats.Add("Eosxd::ext::LS", vid.uid, vid.gid, 1);
        (*parent)[md.md_ino()].set_operation(md.LS);
        eos::Prefetcher::prefetchContainerMDWithChildrenAndWait(gOFS->eosView,
            (IContainerMD::id_t)md.md_ino());
      } else {
        gOFS->MgmStats.Add("Eosxd::ext::GET", vid.uid, vid.gid, 1);
        eos::Prefetcher::prefetchContainerMDAndWait(gOFS->eosView,
            (IContainerMD::id_t)md.md_ino());
      }

      size_t n_attached = 1;
      int retc = 0;

      // retrieve directory meta data
      if (!(retc = FillContainerMD(md.md_ino(), (*parent)[md.md_ino()], vid))) {
        // refresh the cap with the same authid
        FillContainerCAP(md.md_ino(), (*parent)[md.md_ino()], vid,
                         md.authid());

        // store clock
        if (clock) {
          *clock = (*parent)[md.md_ino()].clock();
        }

        if (md.operation() == md.LS) {
          // attach children
          auto map = (*parent)[md.md_ino()].children();
          auto it = map.begin();
          size_t n_caps = 0;
          size_t items_per_lock_cycle = 128;
          size_t items_cycled = 1;

          for (; it != map.end(); ++it) {
            // this is a map by inode
            (*parent)[it->second].set_md_ino(it->second);
            auto child_md = &((*parent)[it->second]);
            items_cycled++;

            if (!(items_cycled % items_per_lock_cycle)) {
              // after <n> entries release the lock and grab again
              rd_ns_lock.Release();
              rd_ns_lock.Grab(gOFS->eosViewRWMutex);
            }

            if (eos::common::FileId::IsFileInode(it->second)) {
              // this is a file
              FillFileMD(it->second, *child_md, vid);
            } else {
              // we don't fill the LS information for the children, just the MD
              child_md->set_operation(md.GET);
              child_md->set_clientuuid(md.clientuuid());
              child_md->set_clientid(md.clientid());
              FillContainerMD(it->second, *child_md, vid);

              if (n_caps < 16) {
                // skip hidden directories
                if (it->first.substr(0, 1) == ".") {
                  // add maximum 16 caps for a listing
                  FillContainerCAP(it->second, *child_md, vid, "", true);
                  n_caps++;
                }
              }

              child_md->clear_operation();
            }
          }

          rd_ns_lock.Release();
          n_attached ++;

          if (n_attached >= 128) {
            std::string rspstream;
            cont.SerializeToString(&rspstream);

            if (!response) {
              // send parent + first 128 children
              gOFS->zMQ->mTask->reply(id, rspstream);
            } else {
              *response += Header(rspstream);
              response->append(rspstream.c_str(), rspstream.size());
            }

            n_attached = 0;
            cont.Clear();
          }
        }

        if (EOS_LOGS_DEBUG) {
          std::string mdout = dump_message(*mdmap);
          eos_debug("\n%s\n", mdout.c_str());
        }
      } else {
        eos_err("ino=%lx errc=%d", (long) md.md_ino(),
                retc);
        return retc;
      }

      (*parent)[md.md_ino()].clear_operation();

      if (n_attached) {
        // send left-over children
        std::string rspstream;
        cont.SerializeToString(&rspstream);

        if (!response) {
          gOFS->zMQ->mTask->reply(id, rspstream);
        } else {
          *response += Header(rspstream);
          response->append(rspstream.c_str(), rspstream.size());
        }
      }

      EXEC_TIMING_END((md.operation() == md.LS) ? "Eosxd::ext::LS" :
                      "Eosxd::ext::GET");
    } else {
      EXEC_TIMING_BEGIN("Eosxd::ext::GET");
      eos_info("ino=%lx get-file/link", (long) md.md_ino());
      Prefetcher::prefetchFileMDAndWait(gOFS->eosView, (IFileMD::id_t)md.md_ino());
      cont.set_type(cont.MD);
      cont.set_ref_inode_(md.md_ino());
      FillFileMD(md.md_ino(), (*cont.mutable_md_()), vid);
      std::string rspstream;
      cont.SerializeToString(&rspstream);

      // store clock
      if (clock) {
        *clock = cont.md_().clock();
      }

      if (!response) {
        // send file meta data
        gOFS->zMQ->mTask->reply(id, rspstream);
      } else {
        *response += Header(rspstream);
        *response += rspstream;
      }

      EXEC_TIMING_END("Eosxd::ext::GET");
    }

    return 0;
  }

  if (md.operation() == md.SET) {
    gOFS->MgmStats.Add("Eosxd::ext::SET", vid.uid, vid.gid, 1);
    EXEC_TIMING_BEGIN("Eosxd::ext::SET");
    uint64_t md_pino = md.md_pino();

    if (!md_pino) {
      // -----------------------------------------------------------------------
      // this can be a creation with an implied capability and the remote inode
      // of the parent directory
      // was not yet send back to the creating client
      // -----------------------------------------------------------------------
      md_pino = InodeFromCAP(md);
    }

    if (!ValidateCAP(md, W_OK | SA_OK, vid)) {
      std::string perm = "W";

      // a CAP might have gone or timedout, let's check again the permissions
      if (((errno == ENOENT) ||
           (errno == EINVAL) ||
           (errno == ETIMEDOUT)) &&
          ValidatePERM(md, perm, vid)) {
        // this can pass on ... permissions are fine
      } else {
        return EPERM;
      }
    }

    enum set_type {
      CREATE, UPDATE, RENAME, MOVE
    } ;
    set_type op;
    uint64_t md_ino = 0;
    bool exclusive = false;

    if (md.type() == md.EXCL) {
      exclusive = true;
    }

    if (S_ISDIR(md.mode())) {
      eos_info("ino=%lx pin=%lx authid=%s set-dir", (long) md.md_ino(),
               (long) md.md_pino(),
               md.authid().c_str());
      eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
      std::shared_ptr<eos::IContainerMD> cmd;
      std::shared_ptr<eos::IContainerMD> pcmd;
      std::shared_ptr<eos::IContainerMD> cpcmd;
      eos::fusex::md mv_md;
      mode_t sgid_mode = 0;

      try {
        if (md.md_ino() && exclusive) {
          eos_err("ino=%lx exists", (long) md.md_ino());
          return EEXIST;
        }

        if (md.md_ino()) {
          if (md.implied_authid().length()) {
            // this is a create on top of an existing inode
            eos_err("ino=%lx exists implied=%s", (long) md.md_ino(),
                    md.implied_authid().c_str());
            return EEXIST;
          }

          op = UPDATE;
          // dir update
          cmd = gOFS->eosDirectoryService->getContainerMD(md.md_ino());
          pcmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino());

          if (cmd->getParentId() != md.md_pino()) {
            // this indicates a directory move
            {
              // we have to check that we have write permission on the source parent
              eos::fusex::md source_md;
              source_md.set_md_pino(cmd->getParentId());
              source_md.set_mode(S_IFDIR);
              std::string perm = "W";

              if (!ValidatePERM(source_md, perm, vid, false)) {
                eos_err("source-ino=%lx no write permission on source directory to do mv ino=%lx",
                        cmd->getParentId(),
                        md.md_ino());
                return EPERM;
              }
            }
            op = MOVE;
            // create a broadcast md object with the authid of the source directory, the target is the standard authid for notification
            mv_md.set_authid(md.mv_authid());
            eos_info("moving %lx => %lx", cmd->getParentId(), md.md_pino());
            cpcmd = gOFS->eosDirectoryService->getContainerMD(cmd->getParentId());
            cpcmd->removeContainer(cmd->getName());
            gOFS->eosView->updateContainerStore(cpcmd.get());
            cmd->setName(md.name());
            std::shared_ptr<eos::IContainerMD> exist_target_cmd;

            try {
              // if the target exists, we have to remove it
              exist_target_cmd = pcmd->findContainer(md.name());

              if (exist_target_cmd) {
                if (exist_target_cmd->getNumFiles() + exist_target_cmd->getNumContainers()) {
                  // that is a fatal error we have to fail that rename
                  eos_err("ino=%lx target exists and is not empty", (long) md.md_ino());
                  return ENOTEMPTY;
                }

                // remove it via the directory service
                gOFS->eosDirectoryService->removeContainer(exist_target_cmd.get());
                pcmd->removeContainer(md.name());
              }
            } catch (eos::MDException& e) {
              // it might not exist, that is fine
            }

            pcmd->addContainer(cmd.get());
            gOFS->eosView->updateContainerStore(pcmd.get());
          }

          if (cmd->getName() != md.name()) {
            // this indicates a directory rename
            op = RENAME;
            eos_info("rename %s=>%s", cmd->getName().c_str(),
                     md.name().c_str());
            gOFS->eosView->renameContainer(cmd.get(), md.name());
          }

          if (pcmd->getMode() & S_ISGID) {
            sgid_mode = S_ISGID;
          }

          md_ino = md.md_ino();
          eos_info("ino=%lx pino=%lx cpino=%lx update-dir",
                   (long) md.md_ino(),
                   (long) md.md_pino(), (long) cmd->getParentId());
        } else {
          // dir creation
          op = CREATE;
          pcmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino());

          if (md.name().substr(0, strlen(EOS_COMMON_PATH_ATOMIC_FILE_PREFIX)) ==
              EOS_COMMON_PATH_ATOMIC_FILE_PREFIX) {
            eos_err("ino=%lx name=%s atomic path is forbidden as a directory name");
            return EPERM;
          }

          if (exclusive && pcmd->findContainer(md.name())) {
            // O_EXCL set on creation -
            eos_err("ino=%lx name=%s exists", md.md_pino(), md.name().c_str());
            return EEXIST;
          }

          eos::IContainerMD::XAttrMap xattrs = pcmd->getAttributes();
          // test to verify this is the culprit of failing all eosxd system tests in the CI
          // if ( (md.attr().find("user.acl") != md.attr().end()) && (xattrs.find("sys.eval.useracl") == xattrs.end()) ) {
          // return EPERM;
          // }
          cmd = gOFS->eosDirectoryService->createContainer();
          cmd->setName(md.name());
          md_ino = cmd->getId();
          pcmd->addContainer(cmd.get());
          eos_info("ino=%lx pino=%lx md-ino=%lx create-dir",
                   (long) md.md_ino(),
                   (long) md.md_pino(),
                   md_ino);

          if (!Cap().Imply(md_ino, md.authid(), md.implied_authid())) {
            eos_err("imply failed for new inode %lx", md_ino);
          }

          // parent attribute inheritance

          for (const auto& elem : xattrs) {
            cmd->setAttribute(elem.first, elem.second);
          }

          sgid_mode = S_ISGID;
        }

        cmd->setName(md.name());
        cmd->setCUid(md.uid());
        cmd->setCGid(md.gid());
        // @todo (apeters): is sgid_mode still needed?
        cmd->setMode(md.mode() | sgid_mode);
        eos::IContainerMD::ctime_t ctime;
        eos::IContainerMD::ctime_t mtime;
        ctime.tv_sec = md.ctime();
        ctime.tv_nsec = md.ctime_ns();
        mtime.tv_sec = md.mtime();
        mtime.tv_nsec = md.mtime_ns();
        cmd->setCTime(ctime);
        cmd->setMTime(mtime);

        for (auto it = md.attr().begin(); it != md.attr().end(); ++it) {
          if ((it->first.substr(0, 3) != "sys") ||
              (it->first == "sys.eos.btime")) {
            cmd->setAttribute(it->first, it->second);
          }
        }

        size_t numAttr = cmd->numAttributes();

        if (op != CREATE &&
            numAttr != md.attr().size()) {    /* an attribute got removed */
          eos::IContainerMD::XAttrMap cmap = cmd->getAttributes();

          for (auto it = cmap.begin(); it != cmap.end(); ++it) {
            if (md.attr().find(it->first) == md.attr().end()) {
              eos_debug("attr %s=%s has been removed", it->first.c_str(),
                        it->second.c_str());
              cmd->removeAttribute(it->first);
              /* if ((--numAttr) == md.attr().size()) break;   would be possible - under a lock! */
            }
          }
        }

        if (op == CREATE) {
          // store the birth time as an extended attribute
          char btime[256];
          snprintf(btime, sizeof(btime), "%lu.%lu", md.btime(), md.btime_ns());
          cmd->setAttribute("sys.eos.btime", btime);
        }

        if (op != UPDATE && md.pmtime()) {
          // store the new modification time for the parent
          eos::IContainerMD::ctime_t pmtime;
          pmtime.tv_sec = md.pmtime();
          pmtime.tv_nsec = md.pmtime_ns();
          pcmd->setMTime(pmtime);
          gOFS->eosDirectoryService->updateStore(pcmd.get());
          pcmd->notifyMTimeChange(gOFS->eosDirectoryService);
        }

        gOFS->eosDirectoryService->updateStore(cmd.get());
        // release the namespace lock before seralization/broadcasting
        lock.Release();
        eos::fusex::response resp;
        resp.set_type(resp.ACK);
        resp.mutable_ack_()->set_code(resp.ack_().OK);
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.mutable_ack_()->set_md_ino(md_ino);
        resp.SerializeToString(response);

        switch (op) {
        case MOVE:
          gOFS->MgmStats.Add("Eosxd::ext::MV", vid.uid, vid.gid, 1);
          break;

        case UPDATE:
          gOFS->MgmStats.Add("Eosxd::ext::UPDATE", vid.uid, vid.gid, 1);
          break;

        case CREATE:
          gOFS->MgmStats.Add("Eosxd::ext::MKDIR", vid.uid, vid.gid, 1);
          break;

        case RENAME:
          gOFS->MgmStats.Add("Eosxd::ext::RENAME", vid.uid, vid.gid, 1);
          break;
        }

        // broadcast this update around
        switch (op) {
        case MOVE:
          Cap().BroadcastRelease(mv_md);

        case UPDATE:
        case CREATE:
        case RENAME:
          Cap().BroadcastRelease(md);
          break;
        }
      } catch (eos::MDException& e) {
        eos_err("ino=%lx err-no=%d err-msg=%s", (long) md.md_ino(),
                e.getErrno(), e.getMessage().str().c_str());
        eos::fusex::response resp;
        resp.set_type(resp.ACK);
        resp.mutable_ack_()->set_code(resp.ack_().PERMANENT_FAILURE);
        resp.mutable_ack_()->set_err_no(e.getErrno());
        resp.mutable_ack_()->set_err_msg(e.getMessage().str().c_str());
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.SerializeToString(response);
      }

      EXEC_TIMING_END("Eosxd::ext::SET");
      return 0;
    }

    if (S_ISREG(md.mode()) || S_ISFIFO(md.mode())) {
      EXEC_TIMING_BEGIN("Eosxd::ext::SET");
      eos_info("ino=%lx pin=%lx authid=%s file", (long) md.md_ino(),
               (long) md.md_pino(),
               md.authid().c_str());
      eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
      std::shared_ptr<eos::IFileMD> fmd;
      std::shared_ptr<eos::IFileMD> ofmd;
      std::shared_ptr<eos::IContainerMD> pcmd;
      std::shared_ptr<eos::IContainerMD> cpcmd;
      uint64_t fid = eos::common::FileId::InodeToFid(md.md_ino());
      md_ino = md.md_ino();
      uint64_t md_pino = md.md_pino();

      try {
        uint64_t clock = 0;
        pcmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino());

        if (md_ino && exclusive) {
          return EEXIST;
        }

        if (md_ino) {
          // file update
          op = UPDATE;
          // dir update
          fmd = gOFS->eosFileService->getFileMD(fid);
          pcmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino());

          if (EOS_LOGS_DEBUG) eos_debug("updating %s => %s ",
                                          fmd->getName().c_str(),
                                          md.name().c_str());

          if (fmd->getContainerId() != md.md_pino()) {
            // this indicates a file move
            op = MOVE;

            if (EOS_LOGS_DEBUG) {
              eos_debug("moving %lx => %lx", fmd->getContainerId(), md.md_pino());
            }

            cpcmd = gOFS->eosDirectoryService->getContainerMD(fmd->getContainerId());
            cpcmd->removeFile(fmd->getName());
            gOFS->eosView->updateContainerStore(cpcmd.get());
            fmd->setName(md.name());
            ofmd = pcmd->findFile(md.name());

            if (ofmd) {
              // the target might exist, so we remove it
              if (EOS_LOGS_DEBUG) {
                eos_debug("removing previous file in move %s", md.name().c_str());
              }

              try {
                pcmd->removeFile(md.name());
                gOFS->eosFileService->removeFile(ofmd.get());
                eos::IQuotaNode* quotanode = gOFS->eosView->getQuotaNode(pcmd.get());

                // free previous quota
                if (quotanode) {
                  quotanode->removeFile(ofmd.get());
                }
              } catch (eos::MDException& e) {
              }
            }

            pcmd->addFile(fmd.get());
            gOFS->eosView->updateFileStore(fmd.get());
            gOFS->eosView->updateContainerStore(pcmd.get());
          } else {
            if (fmd->getName() != md.name()) {
              // this indicates a file rename
              op = RENAME;
              ofmd = pcmd->findFile(md.name());

              if (EOS_LOGS_DEBUG) eos_debug("rename %s [%lx] => %s [%lx]",
                                              fmd->getName().c_str(), fid,
                                              md.name().c_str(),
                                              ofmd ? ofmd->getId() : 0);

              if (ofmd) {
                // the target might exist, so we remove it
                if (EOS_LOGS_DEBUG) {
                  eos_debug("removing previous file in update %s", md.name().c_str());
                }

                try {
                  pcmd->removeFile(md.name());
                  gOFS->eosFileService->removeFile(ofmd.get());
                  eos::IQuotaNode* quotanode = gOFS->eosView->getQuotaNode(pcmd.get());

                  // free previous quota
                  if (quotanode) {
                    quotanode->removeFile(ofmd.get());
                  }
                } catch (eos::MDException& e) {
                }
              }

              gOFS->eosView->renameFile(fmd.get(), md.name());
            }
          }

          eos_info("fid=%lx ino=%lx pino=%lx cpino=%lx update-file",
                   (long) fid,
                   (long) md.md_ino(),
                   (long) md.md_pino(), (long) fmd->getContainerId());
        } else if (strncmp(md.target().c_str(), "////hlnk",
                           8) == 0) {   /* creation of a hard link */
          uint64_t tgt_md_ino = atoll(md.target().c_str() + 8);
          pcmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino());

          if (pcmd->findContainer(
                md.name())) { /* name check protected by eosViewRWMutex above */
            return EEXIST;
          }

          /* fmd is the target file corresponding to tgt_fid, gmd the file corresponding to new name */
          fmd = gOFS->eosFileService->getFileMD(eos::common::FileId::InodeToFid(
                                                  tgt_md_ino));
          std::shared_ptr<eos::IFileMD> gmd = gOFS->eosFileService->createFile();
          int nlink;
          nlink = (fmd->hasAttribute(k_nlink)) ? std::stoi(fmd->getAttribute(
                    k_nlink)) + 1 : 1;

          if (EOS_LOGS_DEBUG) {
            eos_debug("hlnk fid=%#lx target name %s nlink %d create hard link %s",
                      (long) fid, fmd->getName().c_str(), nlink, md.name().c_str());
          }

          fmd->setAttribute(k_nlink, std::to_string(nlink));
          gOFS->eosFileService->updateStore(fmd.get());
          gmd->setAttribute(k_mdino, std::to_string(tgt_md_ino));
          gmd->setName(md.name());

          if (EOS_LOGS_DEBUG) {
            eos_debug("hlnk %s mdino %s %s nlink %s",
                      gmd->getName().c_str(),
                      gmd->getAttribute(k_mdino).c_str(),
                      fmd->getName().c_str(),
                      fmd->getAttribute(k_nlink).c_str());
          }

          pcmd->addFile(gmd.get());
          gOFS->eosFileService->updateStore(gmd.get());
          gOFS->eosView->updateContainerStore(pcmd.get());
          eos::fusex::response resp;
          resp.set_type(resp.ACK);
          resp.mutable_ack_()->set_code(resp.ack_().OK);
          resp.mutable_ack_()->set_transactionid(md.reqid());
          resp.mutable_ack_()->set_md_ino(eos::common::FileId::FidToInode(gmd->getId()));
          // release the namespace lock before serialization/broadcasting
          lock.Release();
          resp.SerializeToString(response);
          struct timespec pt_mtime;
          pt_mtime.tv_sec = md.mtime();
          pt_mtime.tv_nsec = md.mtime_ns();
          gOFS->eosDirectoryService->updateStore(pcmd.get());
          uint64_t clock = 0;
          Cap().BroadcastMD(md, tgt_md_ino, md_pino, clock, pt_mtime);
          return 0;
        } else {
          // file creation
          op = CREATE;

          if (md.name().substr(0, strlen(EOS_COMMON_PATH_ATOMIC_FILE_PREFIX)) ==
              EOS_COMMON_PATH_ATOMIC_FILE_PREFIX) {
            eos_err("name=%s atomic path is forbidden as a filename",
                    md.name().c_str());
            return EPERM;
          }

          unsigned long layoutId = 0;
          unsigned long forcedFsId = 0;
          long forcedGroup = 0;
          XrdOucString space;
          eos::IContainerMD::XAttrMap attrmap = pcmd->getAttributes();
          XrdOucEnv env;
          // retrieve the layout
          Policy::GetLayoutAndSpace("fusex", attrmap, vid, layoutId, space, env,
                                    forcedFsId, forcedGroup);

          if (eos::mgm::FsView::gFsView.IsQuotaEnabled(space.c_str())) {
            // check inode quota here
            long long avail_bytes = 0;
            long long avail_files = 0;

            try {
              eos::IQuotaNode* quotanode = gOFS->eosView->getQuotaNode(pcmd.get());

              if (!Quota::QuotaBySpace(quotanode->getId(),
                                       vid.uid,
                                       vid.gid,
                                       avail_files,
                                       avail_bytes)) {
                if (!avail_files) {
                  eos_err("name=%s out-of-inode-quota uid=%u gid=%u",
                          md.name().c_str(),
                          vid.uid,
                          vid.gid);
                  return EDQUOT;
                }
              }
            } catch (eos::MDException& e) {
            }
          }

          fmd = gOFS->eosFileService->createFile();
          fmd->setName(md.name());
          fmd->setLayoutId(layoutId);
          md_ino = eos::common::FileId::FidToInode(fmd->getId());
          pcmd->addFile(fmd.get());
          eos_info("ino=%lx pino=%lx md-ino=%lx create-file", (long) md.md_ino(),
                   (long) md.md_pino(), md_ino);
        }

        fmd->setName(md.name());
        fmd->setCUid(md.uid());
        fmd->setCGid(md.gid());
        {
          try {
            eos::IQuotaNode* quotanode = gOFS->eosView->getQuotaNode(pcmd.get());

            // free previous quota
            if (quotanode) {
              if (op != CREATE) {
                quotanode->removeFile(fmd.get());
              }

              fmd->setSize(md.size());
              quotanode->addFile(fmd.get());
            } else {
              fmd->setSize(md.size());
            }
          } catch (eos::MDException& e) {
            fmd->setSize(md.size());
          }
        }
        // for the moment we store 9 bits here
        fmd->setFlags(md.mode() & (S_IRWXU | S_IRWXG | S_IRWXO));
        eos::IFileMD::ctime_t ctime;
        eos::IFileMD::ctime_t mtime;
        ctime.tv_sec = md.ctime();
        ctime.tv_nsec = md.ctime_ns();
        mtime.tv_sec = md.mtime();
        mtime.tv_nsec = md.mtime_ns();
        fmd->setCTime(ctime);
        fmd->setMTime(mtime);
        fmd->clearAttributes();
        struct timespec pt_mtime;

        if (op != UPDATE) {
          // update the mtime
          pcmd->setMTime(mtime);
          pt_mtime.tv_sec = mtime.tv_sec;
          pt_mtime.tv_nsec = mtime.tv_nsec;
        } else {
          pt_mtime.tv_sec = pt_mtime.tv_nsec = 0 ;
        }

        for (auto map = md.attr().begin(); map != md.attr().end(); ++map) {
          fmd->setAttribute(map->first, map->second);
        }

        // store the birth time as an extended attribute
        char btime[256];
        snprintf(btime, sizeof(btime), "%lu.%lu", md.btime(), md.btime_ns());
        fmd->setAttribute("sys.eos.btime", btime);
        gOFS->eosFileService->updateStore(fmd.get());

        if (op != UPDATE) {
          // update the mtime
          gOFS->eosDirectoryService->updateStore(pcmd.get());
        }

        // retrieve the clock
        fmd = gOFS->eosFileService->getFileMD(eos::common::FileId::InodeToFid(md_ino),
                                              &clock);
        eos_info("ino=%llx clock=%llx", md_ino, clock);
        // release the namespace lock before serialization/broadcasting
        lock.Release();
        eos::fusex::response resp;
        resp.set_type(resp.ACK);
        resp.mutable_ack_()->set_code(resp.ack_().OK);
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.mutable_ack_()->set_md_ino(md_ino);
        resp.SerializeToString(response);

        switch (op) {
        case MOVE:
          gOFS->MgmStats.Add("Eosxd::ext::MV", vid.uid, vid.gid, 1);
          break;

        case UPDATE:
          gOFS->MgmStats.Add("Eosxd::ext::UPDATE", vid.uid, vid.gid, 1);
          break;

        case CREATE:
          gOFS->MgmStats.Add("Eosxd::ext::CREATE", vid.uid, vid.gid, 1);
          break;

        case RENAME:
          gOFS->MgmStats.Add("Eosxd::ext::RENAME", vid.uid, vid.gid, 1);
          break;
        }

        // broadcast this update around
        switch (op) {
        case UPDATE:
        case CREATE:
        case RENAME:
        case MOVE:
          Cap().BroadcastMD(md, md_ino, md_pino, clock, pt_mtime);
          break;
        }
      } catch (eos::MDException& e) {
        eos_err("ino=%lx err-no=%d err-msg=%s", (long) md.md_ino(),
                e.getErrno(),
                e.getMessage().str().c_str());
        eos::fusex::response resp;
        resp.set_type(resp.ACK);
        resp.mutable_ack_()->set_code(resp.ack_().PERMANENT_FAILURE);
        resp.mutable_ack_()->set_err_no(e.getErrno());
        resp.mutable_ack_()->set_err_msg(e.getMessage().str().c_str());
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.SerializeToString(response);
      }

      EXEC_TIMING_END("Eosxd::ext::SET");
      return 0;
    }

    if (S_ISLNK(md.mode()) || S_ISFIFO(md.mode())) {
      EXEC_TIMING_BEGIN("Eosxd::ext::SET");
      uint64_t clock = 0;
      eos_info("ino=%#lx set-link/fifo %s", (long) md.md_ino(),
               md.name().c_str());
      eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
      std::shared_ptr<eos::IFileMD> fmd;
      std::shared_ptr<eos::IContainerMD> pcmd;
      uint64_t md_pino = md.md_pino();

      try {
        gOFS->MgmStats.Add("Eosxd::ext::CREATELNK", vid.uid, vid.gid, 1);
        // link creation
        pcmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino());
        fmd = pcmd->findFile(md.name());

        if (fmd && exclusive) {
          return EEXIST;
        }

        if (fmd) {
          // file update
          op = UPDATE;
        } else {
          op = CREATE;

          if (md.name().substr(0, strlen(EOS_COMMON_PATH_ATOMIC_FILE_PREFIX)) ==
              EOS_COMMON_PATH_ATOMIC_FILE_PREFIX) {
            eos_err("ino=%lx name=%s atomic path is forbidden as a link/fifo name");
            return EPERM;
          }

          fmd = gOFS->eosFileService->createFile();
        }

        fmd->setName(md.name());

        if (S_ISLNK(md.mode())) {
          fmd->setLink(md.target());
        }

        fmd->setLayoutId(0);
        md_ino = eos::common::FileId::FidToInode(fmd->getId());

        if (op == CREATE) {
          pcmd->addFile(fmd.get());
        }

        eos_info("ino=%lx pino=%lx md-ino=%lx create-link", (long) md.md_ino(),
                 (long) md.md_pino(), md_ino);
        fmd->setCUid(md.uid());
        fmd->setCGid(md.gid());
        fmd->setSize(md.target().length());
        fmd->setFlags(md.mode() & (S_IRWXU | S_IRWXG | S_IRWXO));
        eos::IFileMD::ctime_t ctime;
        eos::IFileMD::ctime_t mtime;
        ctime.tv_sec = md.ctime();
        ctime.tv_nsec = md.ctime_ns();
        mtime.tv_sec = md.mtime();
        mtime.tv_nsec = md.mtime_ns();
        fmd->setCTime(ctime);
        fmd->setMTime(mtime);
        fmd->clearAttributes();

        if (op == CREATE) {
          // store the birth time as an extended attribute
          char btime[256];
          snprintf(btime, sizeof(btime), "%lu.%lu", md.btime(), md.btime_ns());
          fmd->setAttribute("sys.eos.btime", btime);
        }

        struct timespec pt_mtime;

        // update the mtime
        pcmd->setMTime(mtime);

        pt_mtime.tv_sec = mtime.tv_sec;

        pt_mtime.tv_nsec = mtime.tv_nsec;

        gOFS->eosFileService->updateStore(fmd.get());

        gOFS->eosDirectoryService->updateStore(pcmd.get());

        // release the namespace lock before serialization/broadcasting
        lock.Release();

        eos::fusex::response resp;

        resp.set_type(resp.ACK);

        resp.mutable_ack_()->set_code(resp.ack_().OK);

        resp.mutable_ack_()->set_transactionid(md.reqid());

        resp.mutable_ack_()->set_md_ino(md_ino);

        resp.SerializeToString(response);

        Cap().BroadcastMD(md, md_ino, md_pino, clock, pt_mtime);
      } catch (eos::MDException& e) {
        eos_err("ino=%lx err-no=%d err-msg=%s", (long) md.md_ino(),
                e.getErrno(),
                e.getMessage().str().c_str());
        eos::fusex::response resp;
        resp.set_type(resp.ACK);
        resp.mutable_ack_()->set_code(resp.ack_().PERMANENT_FAILURE);
        resp.mutable_ack_()->set_err_no(e.getErrno());
        resp.mutable_ack_()->set_err_msg(e.getMessage().str().c_str());
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.SerializeToString(response);
      }

      EXEC_TIMING_END("Eosxd::ext::SET");
      return 0;
    }
  }

  if (md.operation() == md.DELETE) {
    EXEC_TIMING_BEGIN("Eosxd::ext::RMDIR");

    if (!ValidateCAP(md, D_OK, vid)) {
      std::string perm = "D";

      // a CAP might have gone or timedout, let's check again the permissions
      if (((errno == ENOENT) ||
           (errno == EINVAL) ||
           (errno == ETIMEDOUT)) &&
          ValidatePERM(md, perm, vid)) {
        // this can pass on ... permissions are fine
      } else {
        eos_err("ino=%lx delete has wrong cap");
        return EPERM;
      }
    }

    eos::fusex::response resp;
    resp.set_type(resp.ACK);
    eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
    std::shared_ptr<eos::IContainerMD> cmd;
    std::shared_ptr<eos::IContainerMD> pcmd;
    std::shared_ptr<eos::IFileMD> fmd;
    eos::IFileMD::ctime_t mtime;
    mtime.tv_sec = md.mtime();
    mtime.tv_nsec = md.mtime_ns();

    try {
      pcmd = gOFS->eosDirectoryService->getContainerMD(md.md_pino());

      if (S_ISDIR(md.mode())) {
        cmd = gOFS->eosDirectoryService->getContainerMD(md.md_ino());
      } else {
        fmd = gOFS->eosFileService->getFileMD(eos::common::FileId::InodeToFid(
                                                md.md_ino()));
      }

      pcmd->setMTime(mtime);

      if (S_ISDIR(md.mode())) {
        gOFS->MgmStats.Add("Eosxd::ext::RMDIR", vid.uid, vid.gid, 1);

        // check if this directory is empty
        if (cmd->getNumContainers() || cmd->getNumFiles()) {
          eos::fusex::response resp;
          resp.set_type(resp.ACK);
          resp.mutable_ack_()->set_code(resp.ack_().PERMANENT_FAILURE);
          resp.mutable_ack_()->set_err_no(ENOTEMPTY);
          resp.mutable_ack_()->set_err_msg("directory not empty");
          resp.mutable_ack_()->set_transactionid(md.reqid());
          resp.SerializeToString(response);
          EXEC_TIMING_END("Eosxd::ext::RMDIR");
          return 0;
        }

        eos_info("ino=%lx delete-dir", (long) md.md_ino());
        pcmd->removeContainer(cmd->getName());
        gOFS->eosDirectoryService->removeContainer(cmd.get());
        gOFS->eosDirectoryService->updateStore(pcmd.get());
        pcmd->notifyMTimeChange(gOFS->eosDirectoryService);
        // release the namespace lock before serialization/broadcasting
        lock.Release();
        resp.mutable_ack_()->set_code(resp.ack_().OK);
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.SerializeToString(response);
        Cap().BroadcastRelease(md);
        Cap().BroadcastDeletion(pcmd->getId(), md, cmd->getName());
        Cap().Delete(md.md_ino());
        EXEC_TIMING_END("Eosxd::ext::RMDIR");
        return 0;
      }

      if (S_ISREG(md.mode()) || S_ISFIFO(md.mode())) {
        gOFS->MgmStats.Add("Eosxd::ext::DELETE", vid.uid, vid.gid, 1);
        eos_info("ino=%lx delete-file", (long) md.md_ino());
        eos::IContainerMD::XAttrMap attrmap = pcmd->getAttributes();

        // recycle bin - not for hardlinked files or hardlinks!
        if (attrmap.count(Recycle::gRecyclingAttribute) &&
            (!fmd->hasAttribute(k_mdino)) &&
            (!fmd->hasAttribute(k_nlink))) {
          // translate to a path name and call the complex deletion function
          // this is vulnerable to a hard to trigger race conditions
          std::string fullpath = gOFS->eosView->getUri(fmd.get());
          gOFS->WriteRecycleRecord(fmd);
          gOFS->eosViewRWMutex.UnLockWrite();
          XrdOucErrInfo error;
          (void) gOFS->_rem(fullpath.c_str(), error, vid, "", false, false,
                            false, true);
          gOFS->eosViewRWMutex.LockWrite();
        } else {
          try {
            // handle quota
            eos::IQuotaNode* quotanode = gOFS->eosView->getQuotaNode(pcmd.get());

            if (quotanode) {
              quotanode->removeFile(fmd.get());
            }
          } catch (eos::MDException& e) {
          }

          bool doDelete = true;
          uint64_t tgt_md_ino;

          if (fmd->hasAttribute(
                k_mdino)) {     /* this is a hard link, update reference count on underlying file */
            tgt_md_ino = std::stoll(fmd->getAttribute(k_mdino));
            uint64_t clock;
            /* gmd = the file holding the inode */
            std::shared_ptr<eos::IFileMD> gmd = gOFS->eosFileService->getFileMD(
                                                  eos::common::FileId::InodeToFid(tgt_md_ino), &clock);
            long nlink = std::stol(gmd->getAttribute(k_nlink)) - 1;

            if (nlink >= 0) {
              gmd->setAttribute(k_nlink, std::to_string(nlink));
              gOFS->eosFileService->updateStore(gmd.get());
              eos_info("hlnk nlink update on %s for %s now %ld",
                       gmd->getName().c_str(), fmd->getName().c_str(), nlink);
            } else {    // remove target file as well
              eos_info("hlnk unlink target %s for %s nlink %ld",
                       gmd->getName().c_str(), fmd->getName().c_str(), nlink);
              pcmd->removeFile(gmd->getName());
              gmd->setContainerId(0);
              gmd->unlinkAllLocations();
              gOFS->eosFileService->updateStore(gmd.get());
            }
          } else if (fmd->hasAttribute(
                       k_nlink)) {    /* this is a genuine file, potentially with hard links */
            tgt_md_ino = eos::common::FileId::FidToInode(fmd->getId());
            long nlink = std::stol(fmd->getAttribute(k_nlink)) - 1;

            if (nlink >=
                0) {  // hard links exist, just rename the file so the inode does not disappear
              char nameBuf[256];
              snprintf(nameBuf, sizeof(nameBuf), "...eos.ino...%lx", fmd->getId());
              std::string tmpName = nameBuf;
              fmd->setAttribute(k_nlink, std::to_string(nlink));
              eos_info("hlnk unlink rename %s=>%s new nlink %d",
                       fmd->getName().c_str(), tmpName.c_str(), nlink);
              pcmd->removeFile(tmpName);        // if the target exists, remove it!
              gOFS->eosView->renameFile(fmd.get(), tmpName);
              doDelete = false;
            } else {
              eos_info("hlnk nlink %ld for %s, will be deleted", nlink,
                       fmd->getName().c_str());
            }
          }

          if (doDelete) {
            pcmd->removeFile(fmd->getName());
            fmd->setContainerId(0);
            fmd->unlinkAllLocations();
            gOFS->WriteRmRecord(fmd);
          }

          gOFS->eosFileService->updateStore(fmd.get());
          gOFS->eosDirectoryService->updateStore(pcmd.get());
          pcmd->notifyMTimeChange(gOFS->eosDirectoryService);
        }

        // release the namespace lock before serialization/broadcasting
        lock.Release();
        resp.mutable_ack_()->set_code(resp.ack_().OK);
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.SerializeToString(response);
        Cap().BroadcastRelease(md);
        Cap().BroadcastDeletion(pcmd->getId(), md, md.name());
        Cap().Delete(md.md_ino());
        EXEC_TIMING_END("Eosxd::ext::DELETE");
        return 0;
      }

      if (S_ISLNK(md.mode())) {
        gOFS->MgmStats.Add("Eosxd::ext::DELETELNK", vid.uid, vid.gid, 1);
        EXEC_TIMING_BEGIN("Eosxd::ext::DELETELNK");
        eos_info("ino=%lx delete-link", (long) md.md_ino());
        pcmd->removeFile(fmd->getName());
        fmd->setContainerId(0);
        fmd->unlinkAllLocations();
        gOFS->eosFileService->updateStore(fmd.get());
        gOFS->eosDirectoryService->updateStore(pcmd.get());
        pcmd->notifyMTimeChange(gOFS->eosDirectoryService);
        // release the namespace lock before serialization/broadcasting
        lock.Release();
        resp.mutable_ack_()->set_code(resp.ack_().OK);
        resp.mutable_ack_()->set_transactionid(md.reqid());
        resp.SerializeToString(response);
        Cap().BroadcastRelease(md);
        Cap().BroadcastDeletion(pcmd->getId(), md, md.name());
        Cap().Delete(md.md_ino());
        EXEC_TIMING_END("Eosxd::ext::DELETELNK");
        return 0;
      }
    } catch (eos::MDException& e) {
      resp.mutable_ack_()->set_code(resp.ack_().PERMANENT_FAILURE);
      resp.mutable_ack_()->set_err_no(e.getErrno());
      resp.mutable_ack_()->set_err_msg(e.getMessage().str().c_str());
      resp.mutable_ack_()->set_transactionid(md.reqid());
      resp.SerializeToString(response);
      eos_err("ino=%lx err-no=%d err-msg=%s", (long) md.md_ino(),
              e.getErrno(),
              e.getMessage().str().c_str());
      return 0;
    }
  }

  if (md.operation() == md.GETCAP) {
    gOFS->MgmStats.Add("Eosxd::ext::GETCAP", vid.uid, vid.gid, 1);
    EXEC_TIMING_BEGIN("Eosxd::ext::GETCAP");
    eos::fusex::container cont;
    cont.set_type(cont.CAP);
    eos::fusex::md lmd;
    {
      eos::common::RWMutexReadLock rd_fs_lock(eos::mgm::FsView::gFsView.ViewMutex);
      eos::common::RWMutexReadLock rd_ns_lock(gOFS->eosViewRWMutex);
      // get the meta data
      FillContainerMD((uint64_t) md.md_ino(), lmd, vid);
      lmd.set_clientuuid(md.clientuuid());
      lmd.set_clientid(md.clientid());
      // get the capability
      FillContainerCAP(md.md_ino(), lmd, vid, "");
    }
    // this cap only provides the permissions, but it is not a cap which
    // synchronized the meta data atomically, the client marks a cap locally
    // if he synchronized the contents with it
    *(cont.mutable_cap_()) = lmd.capability();
    std::string rspstream;
    cont.SerializeToString(&rspstream);
    *response += Header(rspstream);
    response->append(rspstream.c_str(), rspstream.size());
    eos_info("cap-issued: id=%lx mode=%x vtime=%lu.%lu uid=%u gid=%u "
             "client-id=%s auth-id=%s errc=%d",
             cont.cap_().id(), cont.cap_().mode(), cont.cap_().vtime(),
             cont.cap_().vtime_ns(), cont.cap_().uid(), cont.cap_().gid(),
             cont.cap_().clientid().c_str(), cont.cap_().authid().c_str(),
             cont.cap_().errc());
    EXEC_TIMING_END("Eosxd::ext::GETCAP");
    return 0;
  }

  if (md.operation() == md.GETLK) {
    gOFS->MgmStats.Add("Eosxd::ext::GETLK", vid.uid, vid.gid, 1);
    EXEC_TIMING_BEGIN("Eosxd::ext::GETLK");
    eos::fusex::response resp;
    resp.set_type(resp.LOCK);
    struct flock lock;
    Locks().getLocks(md.md_ino())->getlk((pid_t) md.flock().pid(), &lock);
    resp.mutable_lock_()->set_len(lock.l_len);
    resp.mutable_lock_()->set_start(lock.l_start);
    resp.mutable_lock_()->set_pid(lock.l_pid);
    eos_info("getlk: ino=%016lx start=%lu len=%ld pid=%u type=%d",
             md.md_ino(),
             lock.l_start,
             lock.l_len,
             lock.l_pid,
             lock.l_type);

    switch (lock.l_type) {
    case F_RDLCK:
      resp.mutable_lock_()->set_type(md.flock().RDLCK);
      break;

    case F_WRLCK:
      resp.mutable_lock_()->set_type(md.flock().WRLCK);
      break;

    case F_UNLCK:
      resp.mutable_lock_()->set_type(md.flock().UNLCK);
      break;
    }

    EXEC_TIMING_END("Eosxd::ext::GETLK");
  }

  if ((md.operation() == md.SETLK) ||
      (md.operation() == md.SETLKW)) {
    EXEC_TIMING_BEGIN((md.operation() == md.SETLKW) ? "Eosxd::ext::SETLKW" :
                      "Eosxd::ext::SETLK");
    eos::fusex::response resp;
    resp.set_type(resp.LOCK);
    int sleep = 0;

    if (md.operation() == md.SETLKW) {
      gOFS->MgmStats.Add("Eosxd::ext::SETLKW", vid.uid, vid.gid, 1);
      sleep = 1;
    } else {
      gOFS->MgmStats.Add("Eosxd::ext::SETLK", vid.uid, vid.gid, 1);
    }

    struct flock lock;

    lock.l_len = md.flock().len();

    lock.l_start = md.flock().start();

    lock.l_pid = md.flock().pid();

    switch (md.flock().type()) {
    case eos::fusex::lock::RDLCK:
      lock.l_type = F_RDLCK;
      break;

    case eos::fusex::lock::WRLCK:
      lock.l_type = F_WRLCK;
      break;

    case eos::fusex::lock::UNLCK:
      lock.l_type = F_UNLCK;
      break;

    default:
      resp.mutable_lock_()->set_err_no(EAGAIN);
      resp.SerializeToString(response);
      return 0;
      break;
    }

    if (lock.l_len == 0) {
      // the infinite lock is represented by -1 in the locking class implementation
      lock.l_len = -1;
    }

    eos_info("setlk: ino=%016lx start=%lu len=%ld pid=%u type=%d",
             md.md_ino(),
             lock.l_start,
             lock.l_len,
             lock.l_pid,
             lock.l_type);

    if (Locks().getLocks(md.md_ino())->setlk(md.flock().pid(), &lock, sleep,
        md.clientuuid())) {
      // lock ok!
      resp.mutable_lock_()->set_err_no(0);
    } else {
      // lock is busy
      resp.mutable_lock_()->set_err_no(EAGAIN);
    }

    resp.SerializeToString(response);
    EXEC_TIMING_END((md.operation() == md.SETLKW) ? "Eosxd::ext::SETLKW" :
                    "Eosxd::ext::SETLK");
    return 0;
  }

  return 0;
}

EOSMGMNAMESPACE_END
