#include "nvme_passthru.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace Embedded;

int Proj3::Open(const std::string &dev, int nsid) {
    nsid_ = nsid;
    int fd = open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st{};
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) { close(fd); return -1; }
    fd_ = fd;
    return 0;
}

int Proj3::io_passthru(uint8_t opcode,
                       uint32_t cdw10, uint16_t nlb,
                       void* buf, uint32_t bytes,
                       uint32_t &result)
{
    struct nvme_passthru_cmd cmd{};
    cmd.opcode = opcode;
    cmd.nsid   = nsid_;
    cmd.cdw10  = cdw10;  // DWORD10 is a key 
    cmd.cdw11  = 0;
    cmd.cdw12  = (uint32_t)nlb;
    cmd.cdw13  = bytes;  // DWORD13 is a value size
    cmd.addr   = reinterpret_cast<uint64_t>(buf);
    cmd.data_len = bytes;
    cmd.timeout_ms = 0; 
    cmd.result = 0;

    int ret = ioctl(fd_, NVME_IOCTL_IO_CMD, &cmd);
    if (ret == 0) {
        result = cmd.result;
    }
    return ret;
}

int Proj3::KeyValuePut(const std::string &key, const std::string &value) {
    void *buf = NULL;
    unsigned int buf_size = value.size();
    unsigned int nlb = ((buf_size-1) / PAGE_SIZE);
    
    buf_size = (nlb + 1) * PAGE_SIZE;
    if (posix_memalign(&buf, PAGE_SIZE, buf_size)) {
        return -ENOMEM;
    }
    memcpy(buf, value.c_str(), value.size());

    int err;
    uint32_t result = UINT32_C(1000);
    uint32_t key_field;
    memcpy(&key_field, key.c_str(), KEY_SIZE);
    
    err = io_passthru(NVME_CMD_KV_PUT, key_field, nlb, buf, buf_size, result);
    if (err < 0 || result != 0) {
        free(buf);
        return -EIO;
    } 

    free(buf);
    return 0;
}

int Proj3::KeyValueGet(const std::string &key, std::string &value) {
    void *buf = NULL; 
    uint32_t buf_size = MAX_BUFLEN; 
    uint32_t nlb = (MAX_BUFLEN-1) / PAGE_SIZE;

    if (posix_memalign(&buf, PAGE_SIZE, buf_size)) {
        return -ENOMEM;
    }
    memset(buf, 0, buf_size);

    int err;
    uint32_t result = UINT32_C(1000);
    uint32_t key_field; 
    memcpy(&key_field, key.c_str(), KEY_SIZE);

    err = io_passthru(NVME_CMD_KV_GET, key_field, nlb, buf, buf_size, result);
    if (err < 0) {
        free(buf);
        return -EIO;
    }

    if (err == ENOSUCHKEY) {
        free(buf);
        return -ENOSUCHKEY;
    }
    
    if (result > 0) { // `result` is a value length
        value = std::string(static_cast<const char*>(buf), (int)result);
    } 
    else {
        value = std::string();
    }

    free(buf);
    return result;
}

