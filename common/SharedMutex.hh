//------------------------------------------------------------------------------
// File: SharedMutex.hh
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2018 CERN/Switzerland                                  *
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

#pragma once
#include "common/Namespace.hh"
#include "common/IRWMutex.hh"

#ifdef __APPLE__
#include <shared_mutex>
#else
#pragma message "WARNING: We are using a copy of the shared mutex to enable the condition variable implementation!!!"
#include "common/shared_mutex"
#endif

EOSCOMMONNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Class SharedMutex - wrapper around std::shared_timed_mutex
//------------------------------------------------------------------------------
class SharedMutex: public IRWMutex
{
public:
  //----------------------------------------------------------------------------
  //! Constructor
  // ---------------------------------------------------------------------------
  SharedMutex() = default;

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~SharedMutex() = default;

  //----------------------------------------------------------------------------
  //! Move constructor
  //----------------------------------------------------------------------------
  SharedMutex(SharedMutex&& other) = delete;

  //----------------------------------------------------------------------------
  //! Move assignment operator
  //----------------------------------------------------------------------------
  SharedMutex& operator=(SharedMutex&& other) = delete;

  //----------------------------------------------------------------------------
  //! Copy constructor
  //----------------------------------------------------------------------------
  SharedMutex(const SharedMutex&) = delete;

  //----------------------------------------------------------------------------
  //! Copy assignment operator
  //----------------------------------------------------------------------------
  SharedMutex& operator=(const SharedMutex&) = delete;

  //----------------------------------------------------------------------------
  //! Lock for read
  //----------------------------------------------------------------------------
  int LockRead() override;

  //----------------------------------------------------------------------------
  //! Unlock a read lock
  //----------------------------------------------------------------------------
  int UnLockRead() override;

  //----------------------------------------------------------------------------
  //! Try to read lock the mutex within the timeout
  //!
  //! @param timeout_ns nano seconds timeout
  //!
  //! @return 0 if successful, otherwise error number
  //----------------------------------------------------------------------------
  int TimedRdLock(uint64_t timeout_ns) override;

  //----------------------------------------------------------------------------
  //! Lock for write
  //----------------------------------------------------------------------------
  int LockWrite() override;

  //----------------------------------------------------------------------------
  //! Unlock a write lock
  //----------------------------------------------------------------------------
  int UnLockWrite() override;

  //----------------------------------------------------------------------------
  //! Try to write lock the mutex within the timeout
  //!
  //! @param timeout_ns nano seconds timeout
  //!
  //! @return 0 if successful, otherwise error number
  //----------------------------------------------------------------------------
  int TimedWrLock(uint64_t timeout_ns) override;

private:
#ifdef __APPLE__
  std::shared_timed_mutex mSharedMutex;
#else
  eos::common::shared_timed_mutex mSharedMutex;
#endif
};

EOSCOMMONNAMESPACE_END
