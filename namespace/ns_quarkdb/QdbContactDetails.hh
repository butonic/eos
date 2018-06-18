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
//! @author Georgios Bitzes <georgios.bitzes@cern.ch>
//! @brief Helper struct to specify all necessary details for contacting a
//!        a QDB cluster
//------------------------------------------------------------------------------

#ifndef EOS_NS_QDB_CONTACT_DETAILS_HH
#define EOS_NS_QDB_CONTACT_DETAILS_HH

#include <qclient/Members.hh>
#include "namespace/Namespace.hh"

EOSNSNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Helper struct to specify all necessary details for contacting a QDB cluster
//------------------------------------------------------------------------------
class QdbContactDetails
{
public:

  //----------------------------------------------------------------------------
  //! Empty constructor
  //----------------------------------------------------------------------------
  QdbContactDetails()
  {

  }

  //----------------------------------------------------------------------------
  //! Check whether object is empty.
  //! It's valid that the password is empty, for now.
  //! TODO(gbitzes): Maybe in the future, disallow contacting unsecured
  //! QDB instances.
  //----------------------------------------------------------------------------
  bool empty() const
  {
    return members.empty();
  }

  qclient::Members members;
  std::string password;
};

EOSNSNAMESPACE_END

#endif