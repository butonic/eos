//------------------------------------------------------------------------------
// File: IConfigEngine.cc
// Author: Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2016 CERN/Switzerland                                  *
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

#include "mgm/config/IConfigEngine.hh"
#include "common/Mapping.hh"
#include "mgm/Access.hh"
#include "mgm/FsView.hh"
#include "mgm/Quota.hh"
#include "mgm/Vid.hh"
#include "mgm/Iostat.hh"
#include "mgm/proc/proc_fs.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/GeoTreeEngine.hh"
#include "mgm/txengine/TransferEngine.hh"
#include "mgm/RouteEndpoint.hh"
#include "mgm/PathRouting.hh"
#include "mgm/Fsck.hh"
#include "common/StringUtils.hh"
#include <sstream>

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//                       **** ICfgEngineChangelog ****
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
//                          **** IConfigEngine ****
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
IConfigEngine::IConfigEngine():
  mChangelog(), mAutosave(false), mBroadcast(true),
  mConfigFile("default")
{}


//------------------------------------------------------------------------------
// XrdOucHash callback function to apply a configuration value
//------------------------------------------------------------------------------
int
IConfigEngine::ApplyEachConfig(const char* key, XrdOucString* val, void* arg)
{
  if (!key || !val) {
    return 0;
  }

  std::ostringstream oss_err;
  XrdOucString* err = reinterpret_cast<XrdOucString*>(arg);
  XrdOucString toenv = val->c_str();

  while (toenv.replace(" ", "&")) {}

  XrdOucEnv envdev(toenv.c_str());
  XrdOucString skey = key;
  std::string sval = val->c_str();
  eos_static_debug("key=%s val=%s", skey.c_str(), val->c_str());

  if (skey.beginswith("fs:")) {
    // Set a filesystem definition
    skey.erase(0, 3);

    if (!FsView::gFsView.ApplyFsConfig(skey.c_str(), sval)) {
      oss_err << "error: failed to apply config "
              << key << " => " << val->c_str() << std::endl;
    }
  } else if (skey.beginswith("global:")) {
    // Set a global configuration
    skey.erase(0, 7);

    if (!FsView::gFsView.ApplyGlobalConfig(skey.c_str(), sval)) {
      oss_err << "error: failed to apply config "
              << key << " => " << val->c_str() << std::endl;
    }

    // Apply the access settings but not the redirection rules
    Access::ApplyAccessConfig(false);
  } else if (skey.beginswith("map:")) {
    // Set a mapping
    skey.erase(0, 4);

    if (!gOFS->AddPathMap(skey.c_str(), sval.c_str(), false)) {
      oss_err << "error: failed to apply config "
              << key << " => " << val->c_str() << std::endl;
    }
  } else if (skey.beginswith("route:")) {
    // Set a routing
    skey.erase(0, 6);
    RouteEndpoint endpoint;

    if (!endpoint.ParseFromString(sval.c_str())) {
      eos_static_err("failed to parse route config %s => %s", key, val->c_str());
      oss_err << "error: failed to parse route config "
              << key << " => " << val->c_str() << std::endl;
    } else {
      if (!gOFS->mRouting->Add(skey.c_str(), std::move(endpoint))) {
        oss_err << "error: failed to apply config "
                << key << " => " << val->c_str() << std::endl;
      }
    }
  } else if (skey.beginswith("quota:")) {
    // Set a quota definition
    skey.erase(0, 6);
    int space_offset = 0;
    int ug_offset = skey.find(':', space_offset + 1);
    int ug_equal_offset = skey.find('=', ug_offset + 1);
    int tag_offset = skey.find(':', ug_equal_offset + 1);

    if ((ug_offset == STR_NPOS) || (ug_equal_offset == STR_NPOS) ||
        (tag_offset == STR_NPOS)) {
      eos_static_err("cannot parse config line key: |%s|", skey.c_str());
      oss_err << "error: cannot parse config line key: "
              << skey.c_str() << std::endl;
      *err = oss_err.str().c_str();
      return 0;
    }

    XrdOucString space(skey, 0, ug_offset - 1);
    XrdOucString ug(skey, ug_offset + 1, ug_equal_offset - 1);
    XrdOucString ugid(skey, ug_equal_offset + 1, tag_offset - 1);
    XrdOucString tag(skey, tag_offset + 1);
    unsigned long long value = strtoll(val->c_str(), 0, 10);
    long id = strtol(ugid.c_str(), 0, 10);

    if (!space.endswith('/')) {
      space += '/';
    }

    if (id > 0 || (ugid == "0")) {
      if (Quota::Create(space.c_str())) {
        if (!Quota::SetQuotaForTag(space.c_str(), tag.c_str(), id, value)) {
          eos_static_err("failed to set quota for id=%s", ugid.c_str());
          oss_err << "error: failed to set quota for id:" << ugid << std::endl;
        }
      } else {
        // This is just ignored ... maybe path is wrong?!
        eos_static_err("failed to create quota for space=%s", space.c_str());
      }
    } else {
      eos_static_err("config id is negative");
      oss_err << "error: illegal id found: " << ugid << std::endl;
    }
  } else if (skey.beginswith("vid:")) {
    // Set a virutal Identity
    int envlen;

    if (!Vid::Set(envdev.Env(envlen), false)) {
      eos_static_err("failed applying config line key: |%s| => |%s|",
                     skey.c_str(), val->c_str());
      oss_err << "error: cannot apply config line key: "
              << skey.c_str() << std::endl;
    }
  } else if (skey.beginswith("geosched:")) {
    skey.erase(0, 9);

    if (!gGeoTreeEngine.setParameter(skey.c_str(), sval.c_str(), -2)) {
      eos_static_err("failed applying config line key: |geosched:%s| => |%s|",
                     skey.c_str(), val->c_str());
      oss_err << "error: failed applying config line key: geosched:"
              << skey.c_str() << std::endl;
    }
  } else if (skey.beginswith("comment")) {
    // Ignore comments
    return 0;
  } else if (skey.beginswith("policy:")) {
    // Set a policy - not used
    return 0;
  } else {
    oss_err << "error: unsupported configuration line: "
            << sval.c_str() << std::endl;
  }

  *err += oss_err.str().c_str();
  return 0;
}

//------------------------------------------------------------------------------
// Check if config key matches filter options as given in opt
//------------------------------------------------------------------------------
bool
IConfigEngine::CheckFilterMatch(XrdOucString& option, const std::string& key)
{
  if (((option.find("v") != STR_NPOS) &&
       (eos::common::startsWith(key, "vid:"))) ||
      ((option.find("f") != STR_NPOS) && (eos::common::startsWith(key, "fs:")))  ||
      ((option.find("q") != STR_NPOS) && (eos::common::startsWith(key, "quota:"))) ||
      ((option.find("p") != STR_NPOS) && (eos::common::startsWith(key, "policy:"))) ||
      ((option.find("c") != STR_NPOS) &&
       (eos::common::startsWith(key, "comment-"))) ||
      ((option.find("g") != STR_NPOS) && (eos::common::startsWith(key, "global:"))) ||
      ((option.find("m") != STR_NPOS) && (eos::common::startsWith(key, "map:"))) ||
      ((option.find("r") != STR_NPOS) && (eos::common::startsWith(key, "route:"))) ||
      ((option.find("s") != STR_NPOS) &&
       (eos::common::startsWith(key, "geosched:")))) {
    return true;
  }

  return false;
}

//------------------------------------------------------------------------------
// Apply a given configuration definition
//------------------------------------------------------------------------------
bool
IConfigEngine::ApplyConfig(XrdOucString& err, bool apply_stall_redirect)
{
  err = "";
  // Cleanup quota map
  (void) Quota::CleanUp();
  {
    eos::common::RWMutexWriteLock wr_lock(eos::common::Mapping::gMapMutex);
    eos::common::Mapping::gUserRoleVector.clear();
    eos::common::Mapping::gGroupRoleVector.clear();
    eos::common::Mapping::gVirtualUidMap.clear();
    eos::common::Mapping::gVirtualGidMap.clear();
    eos::common::Mapping::gAllowedTidentMatches.clear();
  }
  Access::Reset(!apply_stall_redirect);
  {
    eos::common::RWMutexWriteLock wr_view_lock(eos::mgm::FsView::gFsView.ViewMutex);
    XrdSysMutexHelper lock(mMutex);
    // Disable the defaults in FsSpace
    FsSpace::gDisableDefaults = true;

    for(auto it = sConfigDefinitions.begin(); it != sConfigDefinitions.end(); it++) {
      XrdOucString val(it->second.c_str());
      ApplyEachConfig(it->first.c_str(), &val, &err);
    }

    // Enable the defaults in FsSpace
    FsSpace::gDisableDefaults = false;
  }
  Access::ApplyAccessConfig(apply_stall_redirect);
  gOFS->FsCheck.ApplyFsckConfig();
  gOFS->IoStats->ApplyIostatConfig();
  gTransferEngine.ApplyTransferEngineConfig();

  if (err.length()) {
    errno = EINVAL;
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
// Delete a configuration key from the responsible object
//------------------------------------------------------------------------------
void
IConfigEngine::ApplyKeyDeletion(const char* key)
{
  XrdOucString skey = key;
  eos_static_info("key=%s", skey.c_str());

  if (skey.beginswith("fs:")) {
    XrdOucString stdOut;
    XrdOucString stdErr;
    std::string id;
    eos::common::Mapping::VirtualIdentity rootvid;
    eos::common::Mapping::Root(rootvid);
    skey.erase(0, 3);
    int spos1 = skey.find("/", 1);
    int spos2 = skey.find("/", spos1 + 1);
    int spos3 = skey.find("/", spos2 + 1);
    std::string nodename = skey.c_str();
    std::string mountpoint = skey.c_str();
    nodename.erase(spos3);
    mountpoint.erase(0, spos3);
    eos::common::RWMutexWriteLock lock(FsView::gFsView.ViewMutex);
    proc_fs_rm(nodename, mountpoint, id, stdOut, stdErr, rootvid);
  } else  if (skey.beginswith("map:")) {
    skey.erase(0, 4);
    eos::common::RWMutexWriteLock lock(gOFS->PathMapMutex);

    if (gOFS->PathMap.count(skey.c_str())) {
      gOFS->PathMap.erase(skey.c_str());
    }
  } else  if (skey.beginswith("route:")) {
    skey.erase(0, 6);
    gOFS->mRouting->Remove(skey.c_str());
  } else if (skey.beginswith("quota:")) {
    // Remove quota definition
    skey.erase(0, 6);
    int space_offset = 0;
    int ug_offset = skey.find(':', space_offset + 1);
    int ug_equal_offset = skey.find('=', ug_offset + 1);
    int tag_offset = skey.find(':', ug_equal_offset + 1);

    if ((ug_offset == STR_NPOS) || (ug_equal_offset == STR_NPOS) ||
        (tag_offset == STR_NPOS)) {
      eos_static_err("failed to remove quota definition %s", skey.c_str());
      return;
    }

    XrdOucString space(skey, 0, ug_offset - 1);
    XrdOucString ug(skey, ug_offset + 1, ug_equal_offset - 1);
    XrdOucString ugid(skey, ug_equal_offset + 1, tag_offset - 1);
    XrdOucString tag(skey, tag_offset + 1);
    long id = strtol(ugid.c_str(), 0, 10);

    if (id > 0 || (ugid == "0")) {
      if (!Quota::RmQuotaForTag(space.c_str(), tag.c_str(), id)) {
        eos_static_err("failed to remove quota %s for id=%ll", tag.c_str(), id);
      }
    }
  } else if (skey.beginswith("vid:")) {
    // Remove vid entry
    XrdOucString stdOut;
    XrdOucString stdErr;
    int retc = 0;
    XrdOucString vidstr = "mgm.vid.key=";
    vidstr += skey.c_str();
    XrdOucEnv videnv(vidstr.c_str());
    Vid::Rm(videnv, retc, stdOut, stdErr, false);

    if (retc) {
      eos_static_err("failed to remove vid entry for key=%s", skey.c_str());
    }
  } else if (skey.beginswith("policy:") || (skey.beginswith("global:"))) {
    // For policy or global tags don't do anything
  }
}

//------------------------------------------------------------------------------
// Delete configuration values matching the pattern
//------------------------------------------------------------------------------
void
IConfigEngine::DeleteConfigValueByMatch(const char* prefix, const char* match)
{
  XrdOucString smatch = prefix;
  smatch += ":";
  smatch += match;
  XrdSysMutexHelper lock(mMutex);

  auto it = sConfigDefinitions.begin();
  while(it != sConfigDefinitions.end()) {
    if(strncmp(it->first.c_str(), smatch.c_str(), smatch.length()) == 0) {
      it = sConfigDefinitions.erase(it);
    }
    else {
      it++;
    }
  }
}

//------------------------------------------------------------------------------
// Parse configuration from the input given as a string and add it to the
// configuration definition hash.
//------------------------------------------------------------------------------
bool
IConfigEngine::ParseConfig(XrdOucString& inconfig, XrdOucString& err)
{
  int line_num = 0;
  std::string s;
  std::istringstream streamconfig(inconfig.c_str());
  XrdSysMutexHelper lock(mMutex);
  sConfigDefinitions.clear();

  while ((getline(streamconfig, s, '\n'))) {
    line_num++;

    if (s.length()) {
      XrdOucString key = s.c_str();
      int seppos = key.find(" => ");

      if (seppos == STR_NPOS) {
        std::ostringstream oss;
        oss << "parsing error in configuration file line "
            << line_num << ":" <<  s.c_str();
        err = oss.str().c_str();
        errno = EINVAL;
        return false;
      }

      XrdOucString value;
      value.assign(key, seppos + 4);
      key.erase(seppos);

      // Add entry only if key and value are not empty
      if (key.length() && value.length()) {
        eos_notice("setting config key=%s value=%s", key.c_str(), value.c_str());
        sConfigDefinitions[key.c_str()] = value.c_str();
      } else {
        eos_notice("skipping empty config key=%s value=%s", key.c_str(), value.c_str());
      }
    }
  }

  return true;
}

//------------------------------------------------------------------------------
// Dump method for selective configuration printing
//------------------------------------------------------------------------------
bool
IConfigEngine::DumpConfig(XrdOucString& out, XrdOucEnv& filter)
{
  struct PrintInfo pinfo;
  const char* name = filter.Get("mgm.config.file");
  pinfo.out = &out;
  pinfo.option = "vfqcgmrs";

  if (filter.Get("mgm.config.comment") || filter.Get("mgm.config.fs") ||
      filter.Get("mgm.config.global") || filter.Get("mgm.config.map") ||
      filter.Get("mgm.config.route") ||
      filter.Get("mgm.config.policy") || filter.Get("mgm.config.quota") ||
      filter.Get("mgm.config.geosched") || filter.Get("mgm.config.vid")) {
    pinfo.option = "";
  }

  if (filter.Get("mgm.config.comment")) {
    pinfo.option += "c";
  }

  if (filter.Get("mgm.config.fs")) {
    pinfo.option += "f";
  }

  if (filter.Get("mgm.config.global")) {
    pinfo.option += "g";
  }

  if (filter.Get("mgm.config.policy")) {
    pinfo.option += "p";
  }

  if (filter.Get("mgm.config.map")) {
    pinfo.option += "m";
  }

  if (filter.Get("mgm.config.route")) {
    pinfo.option += "r";
  }

  if (filter.Get("mgm.config.quota")) {
    pinfo.option += "q";
  }

  if (filter.Get("mgm.config.geosched")) {
    pinfo.option += "s";
  }

  if (filter.Get("mgm.config.vid")) {
    pinfo.option += "v";
  }

  if (name == 0) {
    XrdSysMutexHelper lock(mMutex);

    for(auto it = sConfigDefinitions.begin(); it != sConfigDefinitions.end(); it++) {
      std::string key = it->first;
      std::string val = it->second;

      eos_static_debug("%s => %s", key.c_str(), val.c_str());

      if (CheckFilterMatch(pinfo.option, key)) {
        out += key.c_str();
        out += " => ";
        out += val.c_str();
        out += "\n";
      }
    }

    while (out.replace("&", " ")) {}
  } else {
    FilterConfig(pinfo, out, name);
  }

  eos::common::StringConversion::SortLines(out);
  return true;
}

//------------------------------------------------------------------------------
// Reset the configuration
//------------------------------------------------------------------------------
void
IConfigEngine::ResetConfig(bool apply_stall_redirect)
{
  mChangelog->AddEntry("reset config", "", "");
  mConfigFile = "";
  (void) Quota::CleanUp();
  {
    eos::common::RWMutexWriteLock wr_lock(eos::common::Mapping::gMapMutex);
    eos::common::Mapping::gUserRoleVector.clear();
    eos::common::Mapping::gGroupRoleVector.clear();
    eos::common::Mapping::gVirtualUidMap.clear();
    eos::common::Mapping::gVirtualGidMap.clear();
    eos::common::Mapping::gAllowedTidentMatches.clear();
  }
  Access::Reset(!apply_stall_redirect);
  gOFS->ResetPathMap();
  gOFS->mRouting->Clear();
  FsView::gFsView.Reset();
  eos::common::GlobalConfig::gConfig.Reset();
  {
    XrdSysMutexHelper lock(mMutex);
    sConfigDefinitions.clear();
  }
  // Load all the quota nodes from the namespace
  Quota::LoadNodes();
}

//------------------------------------------------------------------------------
// Insert comment
//------------------------------------------------------------------------------
void
IConfigEngine::InsertComment(const char* comment)
{
  if (comment) {
    // Store comments as "<unix-tst> <date> <comment>"
    XrdOucString esccomment = comment;
    time_t now = time(0);
    char timestamp[1024];
    sprintf(timestamp, "%lu", now);
    XrdOucString stime = timestamp;
    stime += " ";
    stime += ctime(&now);
    stime.erase(stime.length() - 1);
    stime += " ";

    while (esccomment.replace("\"", "")) {}

    esccomment.insert(stime.c_str(), 0);
    esccomment.insert("\"", 0);
    esccomment.append("\"");

    std::string configkey = SSTR("comment-" << timestamp << ":");
    XrdSysMutexHelper lock(mMutex);
    sConfigDefinitions[configkey] = esccomment.c_str();
  }
}

EOSMGMNAMESPACE_END
