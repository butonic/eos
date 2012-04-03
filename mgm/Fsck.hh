// ----------------------------------------------------------------------
// File: Fsck.hh
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

#ifndef __EOSMGM_FSCK__HH__
#define __EOSMGM_FSCK__HH__

/*----------------------------------------------------------------------------*/
#include "mgm/Namespace.hh"
#include "mgm/FsView.hh"
#include "common/Logging.hh"
/*----------------------------------------------------------------------------*/
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/
#include <google/sparse_hash_map>
#include <google/sparse_hash_set>
#include <sys/types.h>
#include <string>
#include <stdarg.h>
#include <map>
#include <set>
/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

class Fsck {
  // -------------------------------------------------------------
  // ! run's a consistency check over all FST nodes against the MGM namespace
  // -------------------------------------------------------------
private:

  XrdOucString  mLog;
  XrdSysMutex mLogMutex;

  pthread_t mThread;
  bool mRunning;
  
public:
  Fsck();
  ~Fsck();
 
  bool Start();
  bool Stop();

  void PrintOut(XrdOucString &out, XrdOucString option="");
  bool Report(XrdOucString &out, XrdOucString &err, XrdOucString option="", XrdOucString selection="");

  void ClearLog();
  void Log(bool overwrite, const char* msg, ...);

  static void* StaticCheck(void*);
  void* Check();
};

EOSMGMNAMESPACE_END

#endif
