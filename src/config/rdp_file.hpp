#pragma once

#include "config/profile.hpp"

#include <string>

namespace winrdp {

// Parses an .rdp file payload (UTF-8 or UTF-16LE/BE with BOM, Windows mstsc.exe / Thincast format)
bool parse_rdp_content(const std::string& raw_bytes, ConnectionProfile& out_profile, std::string* err = nullptr);
bool load_rdp_file(const std::string& path, ConnectionProfile& out_profile, std::string* err = nullptr);

// Exports a ConnectionProfile to standard Microsoft/Thincast .rdp UTF-8 format
std::string export_rdp_content(const ConnectionProfile& profile);
bool save_rdp_file(const std::string& path, const ConnectionProfile& profile, std::string* err = nullptr);

} // namespace winrdp
