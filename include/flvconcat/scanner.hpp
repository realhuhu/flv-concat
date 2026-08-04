#pragma once

#include "flvconcat/model.hpp"

#include <filesystem>
#include <string>

namespace flvconcat {

bool scan_flv(const std::filesystem::path& path,
              const ScanOptions& options,
              ScanResult& result,
              std::string& error);

} // namespace flvconcat
