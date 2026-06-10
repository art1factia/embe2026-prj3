#include "nvme_passthru.h"
#include <vector>
#include <unordered_map>
#include <random>
#include <cstring>
#include <iostream>
#include <chrono>

using namespace Embedded;

static inline uint8_t pattern_for(uint32_t key, uint64_t gen) {
    return static_cast<uint8_t>((key * 37 + gen * 13) & 0xFF);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " /dev/nvmeXnY [ops=10000] [keyspace=4096] [nsid=1]\n";
        return 1;
    }

    const char* dev = argv[1];
    uint32_t ops       = (argc > 2) ? std::stoul(argv[2]) : 10000;
    uint32_t keyspace  = (argc > 3) ? std::stoul(argv[3]) : 4096;
    int nsid           = (argc > 4) ? std::stoi(argv[4]) : 1;

    Proj3 kv;
    if (kv.Open(dev, nsid) != 0) {
        std::cerr << "Cannot open " << dev << "\n";
        return 2;
    }

    std::mt19937 rng(1234567);
    std::uniform_int_distribution<uint32_t> dist(0, keyspace - 1);

    std::unordered_map<uint32_t, std::string> latest; 

    std::vector<uint8_t> val_buf(PAGE_SIZE); // value size is fixed (4KB), do not modify

    size_t ok = 0, fail = 0;
    size_t no_such_key_cnt = 0;

    auto t_start = std::chrono::high_resolution_clock::now();

    // (1) Random PUTs
    for (uint32_t i = 0; i < ops; i++) {
        uint32_t key = dist(rng);

        uint8_t p = pattern_for(key, i);
        std::string val(val_buf.size(), char(p));

        char key_str[KEY_SIZE + 1]; // key size is fixed (4B), do not modify
        memcpy(key_str, &key, KEY_SIZE);
        key_str[KEY_SIZE] = '\0';

        int ret = kv.KeyValuePut(std::string(key_str, KEY_SIZE), val);
        if (ret != 0) {
            std::cerr << "[ERROR] PUT failed @ key `" << key << "` (SSD internal error)\n";
            fail++;
            continue;
        }

        latest[key] = val;
    }

    // (2) `No such key` test
    uint32_t noexist_key = keyspace + 123; 

    {
        char k[KEY_SIZE + 1];
        memcpy(k, &noexist_key, KEY_SIZE);
        k[KEY_SIZE] = '\0';

        std::string dummy;
        int res = kv.KeyValueGet(std::string(k, KEY_SIZE), dummy);
        if (res == -ENOSUCHKEY) {
            std::cout << "[INFO] GET failed @ key `" << k << "` (No such key)\n";
            no_such_key_cnt++;
        } else {
            std::cerr << "[WARN] No-such-key test did not behave as expected\n";
            fail++;
        }
    }

    // (3) Random GETs
    for (const auto &kvp : latest) {
        uint32_t key = kvp.first;
        const std::string &exp = kvp.second;

        char key_str[KEY_SIZE + 1];
        memcpy(key_str, &key, KEY_SIZE);
        key_str[KEY_SIZE] = '\0';

        std::string out;
        int res = kv.KeyValueGet(std::string(key_str, KEY_SIZE), out);

        if (res < 0) {
            std::cerr << "[ERROR] GET failed @ key `" << key << "` (SSD internal error)\n";
            fail++;
            continue;
        }

        if (out.size() != exp.size()) {
            fail++;
            continue;
        }

        if (memcmp(out.data(), exp.data(), exp.size()) == 0) {
            ok++;
        } else {
            fail++;
        }
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    std::cout << "-----------------------------------------------\n";
    std::cout << " Cosmos+ OpenSSD-Based KV-SSD Benchmark \n";
    std::cout << "-----------------------------------------------\n";
    std::cout << " ops=" << ops << " keyspace=" << keyspace << "\n";
    std::cout << " result: OK=" << ok << " FAIL=" << fail
              << " NO-SUCH-KEY=" << no_such_key_cnt << "\n";
    std::cout << " elapsed: " << ms << " ms  ("
              << (ops * 2.0 / (ms / 1000.0)) << " IOPS est. for PUT+GET )\n";
    std::cout << "-----------------------------------------------\n";

    return (fail == 0) ? 0 : 5;
}

