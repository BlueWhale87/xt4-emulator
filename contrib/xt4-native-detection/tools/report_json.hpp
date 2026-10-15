#pragma once
#include <cstdint>
#include <string>
#include <vector>
std::string json_string(const std::string &);
std::string sha256_bytes(const void *, size_t);
bool atomic_write(const std::string &, const std::string &);
std::string distribution_json(std::vector<uint64_t>);
std::string host_identity_json();
