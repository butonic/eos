// ----------------------------------------------------------------------
// File: Touch.cc
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


// -----------------------------------------------------------------------
// This file is included source code in XrdMgmOfs.cc to make the code more
// transparent without slowing down the compilation time.
// -----------------------------------------------------------------------

/*----------------------------------------------------------------------------*/
int
XrdMgmOfs::_touch(const char* path,
                  XrdOucErrInfo& error,
                  eos::common::Mapping::VirtualIdentity& vid,
                  const char* ininfo)
/*----------------------------------------------------------------------------*/
/*
 * @brief create(touch) a no-replica file in the namespace
 *
 * @param path file to touch
 * @param error error object
 * @param vid virtual identity of the client
 * @param ininfo CGI
 *
 * Access control is not fully done here, just the POSIX write flag is checked,
 * no ACLs ...
 */
/*----------------------------------------------------------------------------*/
{
  EXEC_TIMING_BEGIN("Touch");
  eos_info("path=%s vid.uid=%u vid.gid=%u", path, vid.uid, vid.gid);
  gOFS->MgmStats.Add("Touch", vid.uid, vid.gid, 1);
  // Perform the actual deletion
  errno = 0;
  std::shared_ptr<eos::IFileMD> fmd;
  bool existedAlready = false;

  if (_access(path, W_OK, error, vid, ininfo)) {
    return SFS_ERROR;
  }

  eos::Prefetcher::prefetchFileMDAndWait(gOFS->eosView, path);
  eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);

  try {
    fmd = gOFS->eosView->getFile(path);
    existedAlready = true;
    errno = 0;
  } catch (eos::MDException& e) {
    errno = e.getErrno();
    eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n",
              e.getErrno(), e.getMessage().str().c_str());
  }

  try {
    if (!fmd) {
      fmd = gOFS->eosView->createFile(path, vid.uid, vid.gid);
      fmd->setCUid(vid.uid);
      fmd->setCGid(vid.gid);
      fmd->setCTimeNow();
      fmd->setSize(0);
    }

    fmd->setMTimeNow();
    gOFS->eosView->updateFileStore(fmd.get());
    unsigned long long cid = fmd->getContainerId();
    std::shared_ptr<eos::IContainerMD> cmd =
      gOFS->eosDirectoryService->getContainerMD(cid);
    eos::IFileMD::ctime_t mtime;
    fmd->getMTime(mtime);
    cmd->setMTime(mtime);
    cmd->notifyMTimeChange(gOFS->eosDirectoryService);

    // Check if there is any quota node to be updated
    if(!existedAlready) {
      try {
        eos::IQuotaNode* ns_quota = gOFS->eosView->getQuotaNode(cmd.get());

        if (ns_quota) {
          ns_quota->addFile(fmd.get());
        }
      } catch (const eos::MDException& eq) {
        // no quota node
      }
    }

    gOFS->eosView->updateContainerStore(cmd.get());

    FuseNotificationGuard fuseNotifier(gOFS);
    fuseNotifier.castFile(fmd->getIdentifier());
    fuseNotifier.castContainer(cmd->getIdentifier());
    fuseNotifier.castRefresh(cmd->getIdentifier(), cmd->getParentIdentifier());

    lock.Release();

    errno = 0;
  } catch (eos::MDException& e) {
    errno = e.getErrno();
    eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n",
              e.getErrno(), e.getMessage().str().c_str());
  }

  if (errno) {
    return Emsg("utimes", error, errno, "touch", path);
  }

  EXEC_TIMING_END("Touch");
  return SFS_OK;
}
