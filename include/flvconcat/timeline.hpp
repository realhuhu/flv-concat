#pragma once

#include "flvconcat/model.hpp"

namespace flvconcat {

FilePlan build_timeline(const ScanResult& scan,
                        std::int64_t start_us,
                        const TimelineOptions& options);

} // namespace flvconcat
