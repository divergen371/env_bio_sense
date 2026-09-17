#pragma once

#include "core/io_probe_state.h"

namespace services {

// R01/R02 instrumentation only. Names/outcomes/sites must be string literals
// (or other static-lifetime strings); no path or credential data is captured.
class IoProbe {
public:
    using Stage = core::io_probe::Stage;
    using Metric = core::io_probe::Metric;
    explicit IoProbe(const char* operation, uint64_t thresholdUs = 50000);
    ~IoProbe();
    IoProbe(const IoProbe&) = delete;
    IoProbe& operator=(const IoProbe&) = delete;
    void stage(Stage stage);
    void add(Metric metric, uint32_t delta = 1);
    void sampleHeap();
    void finish(const char* outcome = "returned");

    static bool beginReporter();
    static void storageAcquired(const char* site);
    static void storageReleasing();
    static void loopHeartbeat();

private:
    uint32_t id_ {};
};

} // namespace services
