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

//------------------------------------------------------------------------------
//! @author Elvin-Alin Sindrilaru <esindril@cern.ch>
//! @brief Contants used for the namespace implementation on top of Redis
//------------------------------------------------------------------------------

#ifndef __EOS_NS_REDIS_CONSTANTS_HH__
#define __EOS_NS_REDIS_CONSTANTS_HH__


#include "namespace/Namespace.hh"
#include <string>

EOSNSNAMESPACE_BEGIN

namespace constants
{
  //! Suffix for container metadata in Redis
  static const std::string sContKeySuffix {":bucket_conts"};
  //! Sufix for file metadata in Redis
  static const std::string sFileKeySuffix {":bucket_files"};
  //! Suffix for set of subcontainers in a container
  static const std::string sMapDirsSuffix {":cont_hmap_conts"};
  //! Suffix for set of files in a container
  static const std::string sMapFilesSuffix {":cont_hmap_files"};
  //! Key for set of orphan containers
  static const std::string sSetOrphanCont {"cont_set_orphans"};
  //! Key for set of name conflicts
  static const std::string sSetConflicts {"cont_set_conflicts"};
  //! Key for map containing meta info
  static const std::string sMapMetaInfoKey {"meta_hmap"};
  //! Field last used file id in meta info map
  static const std::string sFirstFreeFid {"first_free_fid"};
  //! Field last used container id in meta info map
  static const std::string sFirstFreeCid {"first_free_cid"};
  //! Set of files that need to be rechecked
  static const std::string sSetCheckFiles {"files_set_check"};
  //! Set of containers that need to be rechecked
  static const std::string sSetCheckConts {"conts_set_check"};
}

EOSNSNAMESPACE_END

#endif // __EOS_NS_REDIS_CONSTANTS_HH__
