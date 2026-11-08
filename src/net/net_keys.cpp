// The keys this machine keeps for online play; see net_keys.hpp.
#include "net/net_keys.hpp"
#include "platform/data_folder.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace conker::net::keys {

namespace {

constexpr size_t kKnownHostsKept = 64;

std::mutex g_mutex;

std::filesystem::path folder() { return conker::data_folder() / "net"; }

// Writes a file only this user can read (on Windows the user's folder's own permissions).
bool write_private(const std::filesystem::path& file, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    {
        std::ofstream create(file, std::ios::trunc);
        if (!create) {
            return false;
        }
    }
    std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    std::ofstream out(file, std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

}  // namespace

seal::KeyPair host_key() {
    std::lock_guard lock(g_mutex);
    seal::KeyPair pair;
    if (const char* test = std::getenv("CONKER_NET_TEST_HOST_KEY"); test != nullptr && seal::key_pair_from_hex(test, &pair)) {
        return pair;
    }
    const std::filesystem::path file = folder() / "host_key";
    {
        std::ifstream in(file);
        std::string text;
        if (in >> text && seal::key_pair_from_hex(text, &pair)) {
            return pair;
        }
    }
    pair = seal::new_key_pair();
    if (!write_private(file, seal::hex(pair.secret) + "\n")) {
        std::printf("[conker] net: could not keep this machine's host key in %s: a new one each time\n",
                    file.string().c_str());
    }
    return pair;
}

std::string key_check(const uint8_t pub[32]) {
    static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // the join codes' (net_codes.cpp)
    const uint64_t bits = seal::fingerprint(pub);
    std::string out;
    for (int i = 7; i >= 0; i--) {
        out += kAlphabet[(bits >> (5 * i)) & 31];
        if (i == 4) {
            out += '-';
        }
    }
    return out;
}

bool known_host(const std::string& address, uint8_t pub[32]) {
    std::lock_guard lock(g_mutex);
    std::ifstream in(folder() / "known_hosts");
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        const size_t space = line.find(' ');
        if (space != std::string::npos && line.substr(0, space) == address) {
            found = seal::key_from_hex(line.substr(space + 1), pub);  // the last line for it counts
        }
    }
    return found;
}

void remember_host(const std::string& address, const uint8_t pub[32]) {
    std::lock_guard lock(g_mutex);
    const std::filesystem::path file = folder() / "known_hosts";
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            const size_t space = line.find(' ');
            if (space != std::string::npos && line.substr(0, space) != address) {
                lines.push_back(line);
            }
        }
    }
    lines.push_back(address + " " + seal::hex(pub));
    while (lines.size() > kKnownHostsKept) {
        lines.erase(lines.begin());  // the one joined longest ago
    }
    std::string text;
    for (const std::string& line : lines) {
        text += line + "\n";
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::trunc);
    out << text;
}

}  // namespace conker::net::keys
