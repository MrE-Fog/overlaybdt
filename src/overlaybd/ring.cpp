/*
 * ring.cpp
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
#include "ring.h"
#include <string.h>
#include <algorithm>
#include <sys/uio.h>
#include "utility.h"
using namespace std;
using namespace photon;

ssize_t RingBuffer::do_read(void *&buf, uint64_t &count, uint64_t end) {
    if (count == 0)
        return 0;
    uint64_t len = min(count, end - m_begin);
    memcpy(buf, m_buf + m_begin, len);
    (char *&)buf += len;
    count -= len;
    m_begin += len;
    return len;
}

ssize_t RingBuffer::do_read(void *buf, size_t count) {
    uint64_t cnt = count;
    while (cnt > 0) {
        if (ensure_not_empty() < 0)
            return count - cnt;
        if (m_begin < m_end) {
            do_read(buf, cnt, m_end);
        } else {
            do_read(buf, cnt, m_capacity);
            if (m_begin == m_capacity)
                m_begin = 0;
        }
        m_cond_pop.notify_all();
    }
    return count;
}

ssize_t RingBuffer::readv(const struct iovec *iov, int iovcnt) {
    ssize_t size = 0;
    scoped_lock lock(m_read_lock);
    for (auto &x : ptr_array(iov, iovcnt)) {
        auto ret = do_read(x.iov_base, x.iov_len);
        if (ret < 0)
            return ret;
        size += ret;
    }
    return size;
}

ssize_t RingBuffer::do_write(const void *&buf, uint64_t &count, uint64_t end) {
    if (count == 0)
        return 0;
    uint64_t len = min(count, end - m_end);
    memcpy(m_buf + m_end, buf, len);
    (const char *&)buf += len;
    count -= len;
    m_end += len;
    return len;
}

ssize_t RingBuffer::do_write(const void *buf, size_t count) {
    uint64_t cnt = count;
    while (cnt > 0) {
        if (ensure_not_full() < 0)
            return count - cnt;
        if (m_end < m_begin) {
            do_write(buf, cnt, m_begin - 1);
        } else {
            auto end = m_capacity;
            if (m_begin == 0)
                end--;
            do_write(buf, cnt, end);
            if (m_end == m_capacity)
                m_end = 0;
        }
        m_cond_push.notify_all();
    }
    return count;
}

ssize_t RingBuffer::writev(const struct iovec *iov, int iovcnt) {
    ssize_t size = 0;
    scoped_lock lock(m_write_lock);
    for (auto &x : ptr_array(iov, iovcnt)) {
        auto ret = do_write(x.iov_base, x.iov_len);
        if (ret < 0)
            return ret;
        size += ret;
    }
    return size;
}