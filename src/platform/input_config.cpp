// conker-recomp-data/input.cfg; see input_config.hpp.
#include "platform/input_config.hpp"
#include "platform/data_folder.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace {

std::filesystem::path config_path() {
    return conker::data_folder() / "input.cfg";
}

// Other files' globals read the file as they are made (frame_input.cpp's mouse aim), maybe before
// this file's would be, so these are made on first use.
std::mutex& config_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::string> read_file() {
    std::map<std::string, std::string> read;
    std::ifstream file(config_path());
    for (std::string line; std::getline(file, line);) {
        const size_t equals = line.find('=');
        if (equals != std::string::npos) {
            read[line.substr(0, equals)] = line.substr(equals + 1);
        }
    }
    return read;
}

// The file's values, read the first time (and by reload); under config_mutex().
std::map<std::string, std::string>& values() {
    static std::map<std::string, std::string> kept = read_file();
    return kept;
}

}  // namespace

namespace conker::input_config {

std::string get(const std::string& key) {
    std::lock_guard lock(config_mutex());
    const auto it = values().find(key);
    return it == values().end() ? "" : it->second;
}

void set(const std::string& key, const std::string& value) {
    std::lock_guard lock(config_mutex());
    if (value.empty()) {
        values().erase(key);
    } else {
        values()[key] = value;
    }
    std::error_code error;
    std::filesystem::create_directories(config_path().parent_path(), error);
    std::ofstream file(config_path());
    for (const auto& [name, kept] : values()) {
        file << name << '=' << kept << '\n';
    }
    if (!file) {
        std::fprintf(stderr, "[conker] could not write %s\n", config_path().string().c_str());
    }
}

void reload() {
    std::lock_guard lock(config_mutex());
    values() = read_file();
}

}  // namespace conker::input_config
