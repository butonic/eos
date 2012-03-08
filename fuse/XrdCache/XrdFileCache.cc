// ----------------------------------------------------------------------
// File: XrdFileCache.cc
// Author: Elvin-Alin Sindrilaru - CERN
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

//------------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <unistd.h>
//------------------------------------------------------------------------------
#include "XrdFileCache.hh"
//------------------------------------------------------------------------------
#include "common/Logging.hh"
//------------------------------------------------------------------------------

XrdFileCache* XrdFileCache::pInstance = NULL;

//------------------------------------------------------------------------------
XrdFileCache*
XrdFileCache::Instance(size_t sizeMax)
{
  if (!pInstance) {
    pInstance  = new XrdFileCache(sizeMax);
    pInstance->Init();
  }

  return pInstance;
}


//------------------------------------------------------------------------------
XrdFileCache::XrdFileCache(size_t sizeMax):
  cacheSizeMax(sizeMax),
  indexFile(10)
{
  pthread_rwlock_init(&keyMgmLock, NULL);
}


//------------------------------------------------------------------------------
void
XrdFileCache::Init()
{
  usedIndexQueue = new ConcurrentQueue<int>();
  cacheImpl = new CacheImpl(cacheSizeMax, this);

  //start worker thread
  threadStart(writeThread, XrdFileCache::writeThreadProc);
}


//------------------------------------------------------------------------------
XrdFileCache::~XrdFileCache()
{
  void* ret;
  //add sentinel object to queue => kill worker thread
  cacheImpl->killWriteThread();
  pthread_join(writeThread, &ret);

  delete cacheImpl;
  delete usedIndexQueue;
  pthread_rwlock_destroy(&keyMgmLock);
  return;
}


//------------------------------------------------------------------------------
int
XrdFileCache::threadStart(pthread_t& thread, ThreadFn f)
{
  return pthread_create(&thread, NULL, f, (void*) this);
}


//------------------------------------------------------------------------------
void*
XrdFileCache::writeThreadProc(void* arg)
{
  XrdFileCache* pfc = static_cast<XrdFileCache*>(arg);
  pfc->cacheImpl->runThreadWrites();
  eos_static_debug("stopped writer thread");
  return (void*) pfc;
}


//------------------------------------------------------------------------------
void
XrdFileCache::setCacheSize(size_t rsMax, size_t wsMax)
{
  cacheImpl->setSize(rsMax);
}


//------------------------------------------------------------------------------
FileAbstraction*
XrdFileCache::getFileObj(unsigned long inode, bool getNew)
{
  int key = -1;
  FileAbstraction* fRet = NULL;

  pthread_rwlock_rdlock(&keyMgmLock);    //read lock

  std::map<unsigned long, FileAbstraction*>::iterator iter = fileInodeMap.find(inode);

  if (iter != fileInodeMap.end()) {
    fRet = iter->second;
    key = fRet->getId();
  }
  else if (getNew)
  {
    pthread_rwlock_unlock(&keyMgmLock);  //unlock
    pthread_rwlock_wrlock(&keyMgmLock);  //write lock
    if (indexFile >= maxIndexFiles) {
      while (!usedIndexQueue->try_pop(key)) {
        cacheImpl->removeBlock();
      }
      
      fRet = new FileAbstraction(key, inode);
      fileInodeMap.insert(std::pair<unsigned long, FileAbstraction*>(inode, fRet));
    } else {
      key = indexFile;
      fRet = new FileAbstraction(key, inode);
      fileInodeMap.insert(std::pair<unsigned long, FileAbstraction*>(inode, fRet));
      indexFile++;
    }
  }
  else {
    pthread_rwlock_unlock(&keyMgmLock);  //unlock
    return 0;
  }

  //increase the number of references to this file
  fRet->incrementNoReferences();
  pthread_rwlock_unlock(&keyMgmLock);  //unlock

  eos_static_debug("inode=%lu, key=%i", inode, key);

  return fRet;
}


//------------------------------------------------------------------------------
void
XrdFileCache::submitWrite(unsigned long inode, int filed, void* buf,
                          off_t offset, size_t length)
{
  size_t nwrite;
  long long int key;
  off_t writtenOffset = 0;

  FileAbstraction* fAbst = getFileObj(inode, true);
  
  while (((offset % CacheEntry::getMaxSize()) + length) > CacheEntry::getMaxSize()) {
    nwrite = CacheEntry::getMaxSize() - (offset % CacheEntry::getMaxSize());
    key = fAbst->generateBlockKey(offset);
    eos_static_debug("(1) off=%zu, len=%zu", offset, nwrite);
    cacheImpl->addWrite(filed, key, static_cast<char*>(buf) + writtenOffset, offset, nwrite, fAbst);

    offset += nwrite;
    length -= nwrite;
    writtenOffset += nwrite;
  }

  if (length != 0) {
    nwrite = length;
    key = fAbst->generateBlockKey(offset);
    eos_static_debug("(2) off=%zu, len=%zu", offset, nwrite);
    cacheImpl->addWrite(filed, key, static_cast<char*>(buf) + writtenOffset, offset, nwrite, fAbst);
    writtenOffset += nwrite;
  }

  fAbst->decrementNoReferences();
  return;
}


//------------------------------------------------------------------------------
size_t
XrdFileCache::getRead(FileAbstraction* fAbst, int filed, void* buf,
                      off_t offset, size_t length)
{
  bool found = true;
  long long int key;
  size_t nread;
  off_t readOffset = 0;

  //read bigger than block size, break in smaller blocks
  while (((offset % CacheEntry::getMaxSize()) + length) > CacheEntry::getMaxSize()) {
    nread = CacheEntry::getMaxSize() - (offset % CacheEntry::getMaxSize());
    key = fAbst->generateBlockKey(offset);
    eos_static_debug("(1) off=%zu, len=%zu", offset, nread);
    found = cacheImpl->getRead(key, static_cast<char*>(buf) + readOffset, offset, nread, fAbst);

    if (!found) {
      return 0;
    }

    offset += nread;
    length -= nread;
    readOffset += nread;
  }

  if (length != 0) {
    nread = length;
    key = fAbst->generateBlockKey(offset);
    eos_static_debug("(2) off=%zu, len=%zu", offset, nread);
    found = cacheImpl->getRead(key, static_cast<char*>(buf) + readOffset, offset, nread, fAbst);

    if (!found) {
      return 0;
    }

    readOffset += nread;
  }

  return readOffset;
}


//------------------------------------------------------------------------------
size_t
XrdFileCache::putRead(FileAbstraction* fAbst, int filed, void* buf,
                      off_t offset, size_t length)
{
  size_t nread;
  long long int key;
  off_t readOffset = 0;

  //read bigger than block size, break in smaller blocks
  while (((offset % CacheEntry::getMaxSize()) + length) > CacheEntry::getMaxSize()) {
    nread = CacheEntry::getMaxSize() - (offset % CacheEntry::getMaxSize());
    key = fAbst->generateBlockKey(offset);
    eos_static_debug("(1) off=%zu, len=%zu key=%lli", offset, nread, key);
    cacheImpl->addRead(filed, key, static_cast<char*>(buf) + readOffset, offset, nread, fAbst);
    offset += nread;
    length -= nread;
    readOffset += nread;
  }

  if (length != 0) {
    nread = length;
    key = fAbst->generateBlockKey(offset);
    eos_static_debug("(2) off=%zu, len=%zu key=%lli", offset, nread, key);
    cacheImpl->addRead(filed, key, static_cast<char*>(buf) + readOffset, offset, nread, fAbst);
    readOffset += nread;
  }

  return readOffset;
}

/*
//------------------------------------------------------------------------------
size_t
XrdFileCache::getReadV(unsigned long inode, int filed, void* buf,
off_t* offset, size_t* length, int nbuf)
{
size_t ret = 0;
char* ptrBuf = static_cast<char*>(buf);
long long int key;
CacheEntry* pEntry = NULL;
FileAbstraction* fAbst = getFileObj(inode, true);

for (int i = 0; i < nbuf; i++) {
key = fAbst->GenerateBlockKey(offset[i]);
pEntry = cacheImpl->getRead(key, fAbst);

if (pEntry && (pEntry->GetLength() == length[i])) {
ptrBuf = (char*)memcpy(ptrBuf, pEntry->GetDataBuffer(), pEntry->GetLength());
ptrBuf += length[i];
ret += length[i];
} else break;
}

return ret;
}


//------------------------------------------------------------------------------
void
XrdFileCache::putReadV(unsigned long inode, int filed, void* buf,
off_t* offset, size_t* length, int nbuf)
{
char* ptrBuf = static_cast<char*>(buf);
long long int key;
CacheEntry* pEntry = NULL;
FileAbstraction* fAbst = getFileObj(inode, true);

for (int i = 0; i < nbuf; i++) {
pEntry = cacheImpl->getRecycledBlock(filed, ptrBuf, offset[i], length[i], fAbst);
key = fAbst->GenerateBlockKey(offset[i]);
cacheImpl->Insert(key, pEntry);
ptrBuf += length[i];
}

return;
}
*/


//------------------------------------------------------------------------------
bool
XrdFileCache::removeFileInode(unsigned long inode, bool strongConstraint)
{
  bool doDeletion = false;
  eos_static_debug("inode=%lu", inode);
  FileAbstraction* ptr =  NULL;

  pthread_rwlock_wrlock(&keyMgmLock);   //write lock
  std::map<unsigned long, FileAbstraction*>::iterator iter = fileInodeMap.find(inode);

  if (iter != fileInodeMap.end()) {
    ptr = static_cast<FileAbstraction*>((*iter).second);

    if (strongConstraint) {  //strong constraint
      doDeletion = (ptr->getSizeRdWr() == 0) && (ptr->getNoReferences() == 0);
    }
    else { //weak constraint
      doDeletion = (ptr->getSizeRdWr() == 0) && (ptr->getNoReferences() <= 1);
    }
    if (doDeletion) {
      //remove file from mapping
      int id = ptr->getId();
      usedIndexQueue->push(id);
      delete ptr;
      fileInodeMap.erase(iter);
    }
  }

  pthread_rwlock_unlock(&keyMgmLock);   //unlock
  return doDeletion;
}


//------------------------------------------------------------------------------
ConcurrentQueue<error_type>&
XrdFileCache::getErrorQueue(unsigned long inode)
{
  ConcurrentQueue<error_type>* tmp = NULL;
  FileAbstraction* fAbst = getFileObj(inode, false);

  if (fAbst) {
    *tmp = fAbst->getErrorQueue();
    fAbst->decrementNoReferences();
  }

  return *tmp;
}

//------------------------------------------------------------------------------
void
XrdFileCache::waitFinishWrites(FileAbstraction* fAbst)
{
  if (fAbst->getSizeWrites() != 0) {
    cacheImpl->flushWrites(fAbst);
    fAbst->waitFinishWrites();
    if (!fAbst->isInUse(false)) {
      removeFileInode(fAbst->getInode(), false);
    }
  }

  return;
}


//------------------------------------------------------------------------------
void
XrdFileCache::waitFinishWrites(unsigned long inode)
{
  FileAbstraction* fAbst = getFileObj(inode, false);
  
  if (fAbst && (fAbst->getSizeWrites() != 0)) {
    cacheImpl->flushWrites(fAbst);
    fAbst->waitFinishWrites();
    if (!fAbst->isInUse(false)) {
      if (removeFileInode(fAbst->getInode(), false)) {
        return;
      }
    }
  }

  if (fAbst) {
    fAbst->decrementNoReferences();
  }
  return;
}
