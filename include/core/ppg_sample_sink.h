#pragma once

#include <cstdint>

namespace core {

struct PpgRawSample {
    uint32_t red {};
    uint32_t ir {};
    uint32_t timestampMs {};
    uint32_t logicalIndex {};
};

class IPpgSampleSink {
public:
    virtual ~IPpgSampleSink() = default;
    virtual void onPpgSample(const PpgRawSample& sample) = 0;
};

} // namespace core
