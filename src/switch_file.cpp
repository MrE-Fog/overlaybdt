/*
 * switch_file.cpp
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

#include <fcntl.h>
#include <mutex>
#include "overlaybd/alog-audit.h"
#include "overlaybd/alog-stdstring.h"
#include "overlaybd/alog.h"
#include "overlaybd/fs/localfs.h"
#include "overlaybd/fs/zfile/zfile.h"
#include "overlaybd/photon/thread.h"
#include "switch_file.h"

using namespace std;

namespace FileSystem {

#define FORWARD(func)                                                                              \
    check_switch();                                                                                \
    ++io_count;                                                                                    \
    DEFER({ --io_count; });                                                                        \
    return m_file->func;

class SwitchFile : public ISwitchFile {
public:
    int state; /* 0. normal state; 1. ready to switch; 2. in processing */
    int io_count;
    bool local_path;
    IFile *m_file = nullptr;
    IFile *m_old = nullptr;
    string m_filepath;

    SwitchFile(IFile *source) : m_file(source) {
        state = 0;
        io_count = 0;
        local_path = false;
    };

    virtual ~SwitchFile() override {
        if (!m_file)
            safe_delete(m_file);
        if (!m_old)
            safe_delete(m_old);
    }

    void set_switch_file(const char *filepath) {
        m_filepath = filepath;
        state = 1;
    }

    int do_switch() {
        int flags = O_RDONLY;
        // TODO support libaio
        auto local_file = open_localfile_adaptor(m_filepath.c_str(), flags, 0644, 0);
        if (local_file == nullptr) {
            LOG_ERROR_RETURN(0, -1, "failed to open commit file, path: `, error: `(`)", m_filepath,
                             errno, strerror(errno));
        }

        int is_zf = ZFile::is_zfile(local_file);
        if (is_zf == -1) {
            delete local_file;
            LOG_ERROR_RETURN(0, -1, "is_zfile(sf) path:`, error: `(`)", m_filepath, errno,
                             strerror(errno));
        }
        if (is_zf == 1) {
            auto zf = ZFile::zfile_open_ro(local_file, false, true);
            if (!zf) {
                delete local_file;
                LOG_ERROR_RETURN(0, -1, "failed to open zfile. path:`, errno:`", m_filepath, errno);
            }
            local_file = zf;
        }

        LOG_INFO("switch to localfile '`' success.", m_filepath);
        m_old = m_file;
        m_file = local_file;
        local_path = true;
        return 0;
    }

    int check_switch() {
        if (state == 0) {
            return 0;
        }
        if (state == 2) {
            while (state != 0) {
                photon::thread_usleep(1000);
            }
            return 0;
        }
        // state == 1
        state = 2;
        while (io_count > 0) {
            photon::thread_usleep(1000);
        }
        // set set to 0, even do_switch failed
        do_switch();
        state = 0;
        return 0;
    }

    virtual int close() override {
        FORWARD(close());
    }
    virtual ssize_t read(void *buf, size_t count) override {
        FORWARD(read(buf, count));
    }
    virtual ssize_t readv(const struct iovec *iov, int iovcnt) override {
        FORWARD(readv(iov, iovcnt));
    }
    virtual ssize_t write(const void *buf, size_t count) override {
        FORWARD(write(buf, count));
    }
    virtual ssize_t writev(const struct iovec *iov, int iovcnt) override {
        FORWARD(writev(iov, iovcnt));
    }
    virtual ssize_t pread(void *buf, size_t count, off_t offset) override {
        if (local_path) {
            SCOPE_AUDIT("file:pread", AU_FILEOP(m_filepath, offset, count));
            FORWARD(pread(buf, count, offset));
        } else {
            FORWARD(pread(buf, count, offset));
        }
    }
    virtual ssize_t pwrite(const void *buf, size_t count, off_t offset) override {
        FORWARD(pwrite(buf, count, offset));
    }
    virtual ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        FORWARD(preadv(iov, iovcnt, offset));
    }
    virtual ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        FORWARD(pwritev(iov, iovcnt, offset));
    }
    virtual off_t lseek(off_t offset, int whence) override {
        FORWARD(lseek(offset, whence));
    }
    virtual int fstat(struct stat *buf) override {
        FORWARD(fstat(buf));
    }
    virtual IFileSystem *filesystem() override {
        FORWARD(filesystem());
    }
    virtual int fsync() override {
        FORWARD(fsync());
    }
    virtual int fdatasync() override {
        FORWARD(fdatasync());
    }
    virtual int sync_file_range(off_t offset, off_t nbytes, unsigned int flags) override {
        FORWARD(sync_file_range(offset, nbytes, flags));
    }
    virtual int fchmod(mode_t mode) override {
        FORWARD(fchmod(mode));
    }
    virtual int fchown(uid_t owner, gid_t group) override {
        FORWARD(fchown(owner, group));
    }
    virtual int ftruncate(off_t length) override {
        FORWARD(ftruncate(length));
    }
    virtual int fallocate(int mode, off_t offset, off_t len) override {
        FORWARD(fallocate(mode, offset, len));
    }
};

ISwitchFile *new_switch_file(IFile *source) {
    return new SwitchFile(source);
};
} // namespace FileSystem