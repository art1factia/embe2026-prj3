#pragma once
#include <string>
#include <vector>
#include <cstdint>

#define ENOSUCHKEY 0x7C1

// do not modify below variables
const unsigned int KEY_SIZE = 4;
const unsigned int PAGE_SIZE = 4096;
const unsigned int MAX_BUFLEN = 16384; 

namespace Embedded {

    enum nvme_opcode {
        NVME_CMD_WRITE                 = 0x01,
        NVME_CMD_READ                  = 0x02,
        NVME_CMD_KV_PUT                = 0xA0,
        NVME_CMD_KV_GET                = 0xA1,
    };

    class Proj3 {
    public:
        Proj3() : fd_(-1), nsid_(1) {}
        int Open(const std::string &dev, int nsid = 1);
        int KeyValuePut(const std::string &key, const std::string &value);
        int KeyValueGet(const std::string &key, std::string &value);

    private:
        int fd_;
        int nsid_;
        int io_passthru(uint8_t opcode,
                        uint32_t cdw10, uint16_t nlb,
                        void* buf, uint32_t bytes,
                        uint32_t &result);
    };
}

