/*
 * cache.cpp
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */
#include "cache.h"
#include "../../alog.h"
#include "../../io-alloc.h"
#include "../../iovector.h"

#include "frontend/cached_file.h"
#include "pool_store.h"
#include "full_file_cache/cache_pool.h"

namespace FileSystem {
ICacheStore::try_preadv_result ICacheStore::try_preadv(const struct iovec *iov, int iovcnt,
                                                       off_t offset) {
    try_preadv_result rst;
    iovector_view view((iovec *)iov, iovcnt);
    rst.iov_sum = view.sum();
    auto q = queryRefillRange(offset, rst.iov_sum);
    if (q.second == 0) { // no need to refill
        rst.refill_size = 0;
        rst.size = this->preadv(iov, iovcnt, offset);
    } else {
        rst.refill_size = q.second;
        rst.refill_offset = q.first;
    }
    return rst;
}
ICacheStore::try_preadv_result ICacheStore::try_preadv_mutable(struct iovec *iov, int iovcnt,
                                                               off_t offset) {
    return try_preadv(iov, iovcnt, offset);
}
ssize_t ICacheStore::preadv(const struct iovec *iov, int iovcnt, off_t offset) {
    SmartCloneIOV<32> ciov(iov, iovcnt);
    return preadv_mutable(ciov.iov, iovcnt, offset);
}
ssize_t ICacheStore::preadv_mutable(struct iovec *iov, int iovcnt, off_t offset) {
    return preadv(iov, iovcnt, offset);
}
ssize_t ICacheStore::pwritev(const struct iovec *iov, int iovcnt, off_t offset) {
    SmartCloneIOV<32> ciov(iov, iovcnt);
    return pwritev_mutable(ciov.iov, iovcnt, offset);
}
ssize_t ICacheStore::pwritev_mutable(struct iovec *iov, int iovcnt, off_t offset) {
    return pwritev(iov, iovcnt, offset);
}

ICachedFileSystem *new_full_file_cached_fs(IFileSystem *srcFs, IFileSystem *mediaFs,
                                           uint64_t refillUnit, uint64_t capacityInGB,
                                           uint64_t periodInUs, uint64_t diskAvailInBytes,
                                           IOAlloc *allocator) {
    if (refillUnit % 4096 != 0) {
        LOG_ERROR_RETURN(EINVAL, nullptr, "refill Unit need to be aligned to 4KB")
    }
    if (!allocator) {
        allocator = new IOAlloc;
    }
    Cache::FileCachePool *pool = nullptr;
    pool =
        new ::Cache::FileCachePool(mediaFs, capacityInGB, periodInUs, diskAvailInBytes, refillUnit);
    pool->Init();
    return new_cached_fs(srcFs, pool, 4096, refillUnit, allocator);
}

ICacheStore *ICachePool::open(std::string_view filename, int flags, mode_t mode) {
    ICacheStore *cache_store = nullptr;
    auto it = m_stores.find(filename);
    if (it != m_stores.end())
        cache_store = it->second;
    if (cache_store == nullptr) {
        cache_store = this->do_open(filename, flags, mode);
        if (nullptr == cache_store) {
            LOG_ERRNO_RETURN(0, nullptr, "fileCachePool_ open file failed, name : `",
                             filename.data());
        }
        m_stores.emplace(filename, cache_store);
        auto it = m_stores.find(filename);
        std::string_view map_key = it->first;
        cache_store->set_pathname(map_key);
        cache_store->set_pool(this);
    }
    cache_store->add_ref();
    return cache_store;
}

int ICachePool::store_release(ICacheStore *store) {
    auto iter = m_stores.find(store->get_pathname());
    if (iter == m_stores.end()) {
        LOG_ERROR_RETURN(0, -1, "try to erase an unexist store from map m_stores , name : `",
                         store->get_pathname().data());
    }
    m_stores.erase(iter);
    return 0;
}
} // namespace FileSystem