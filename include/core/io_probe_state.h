#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace core {
namespace io_probe {

// Clock and synchronization are supplied by the ESP adapter. No allocation,
// filesystem, logger or FreeRTOS dependencies in the bookkeeping itself.
enum class Stage : uint8_t {
    Enter, Wait, Open, Scan, Close, Snapshot, Filter, Sort, Json, Send,
    Prepare, WalRead, TxStart, Inspect, Read, Write, Flush, Reopen,
    Verify, Consume, TxFinish, Mount, Count
};
enum class Metric : uint8_t { Entries, Directories, Files, Bytes, Records, Count };

struct Record {
    uint32_t id {};
    const char* operation {};
    char task[16] {};
    uint32_t priority {};
    int core {};
    Stage stage {Stage::Enter};
    uint64_t startedUs {};
    uint64_t stageSinceUs {};
    uint64_t endedUs {};
    uint64_t lastReportUs {};
    uint64_t thresholdUs {};
    uint64_t stageUs[static_cast<size_t>(Stage::Count)] {};
    uint32_t metrics[static_cast<size_t>(Metric::Count)] {};
    uint32_t minHeap {UINT32_MAX};
    const char* outcome {"returned"};
};

struct Owner {
    uintptr_t taskId {};
    char task[16] {};
    const char* site {};
    uint64_t sinceUs {};
    uint32_t depth {};
};

template <size_t Active = 12, size_t Completed = 8>
class State {
public:
    static_assert(Active > 0 && Completed > 0, "diagnostic buffers must be bounded and nonempty");

    uint32_t begin(uint64_t now, const char* operation, const char* task,
                   uint32_t priority, int cpu, uint64_t thresholdUs) {
        for (Record& record : active_) {
            if (record.id != 0) continue;
            record = Record();
            if (++nextId_ == 0) ++nextId_;
            record.id = nextId_;
            record.operation = operation;
            copyTask(record.task, task);
            record.priority = priority;
            record.core = cpu;
            record.startedUs = record.stageSinceUs = now;
            record.thresholdUs = thresholdUs;
            return record.id;
        }
        ++droppedActive;
        return 0; // Never wait or evict an in-flight operation for telemetry.
    }

    void stage(uint32_t id, Stage next, uint64_t now) {
        Record* record = find(id);
        if (!record || next >= Stage::Count) return;
        record->stageUs[static_cast<size_t>(record->stage)] += now - record->stageSinceUs;
        record->stageSinceUs = now;
        record->stage = next;
    }

    void metric(uint32_t id, Metric metric, uint32_t delta) {
        Record* record = find(id);
        if (record && metric < Metric::Count) record->metrics[static_cast<size_t>(metric)] += delta;
    }

    void heap(uint32_t id, uint32_t bytes) {
        Record* record = find(id);
        if (record && bytes < record->minHeap) record->minHeap = bytes;
    }

    void end(uint32_t id, uint64_t now, const char* outcome) {
        Record* record = find(id);
        if (!record) return;
        record->stageUs[static_cast<size_t>(record->stage)] += now - record->stageSinceUs;
        record->endedUs = now;
        record->outcome = outcome;
        if (now - record->startedUs >= record->thresholdUs) {
            if (count_ < Completed) {
                completed_[(head_ + count_) % Completed] = *record;
                ++count_;
            } else {
                ++droppedCompleted;
            }
        }
        record->id = 0;
    }

    bool pop(Record& out) {
        if (count_ == 0) return false;
        out = completed_[head_];
        head_ = (head_ + 1) % Completed;
        --count_;
        return true;
    }

    bool stalled(size_t slot, uint64_t now, Record& out) {
        if (slot >= Active) return false;
        Record& record = active_[slot];
        if (record.id == 0 || now - record.startedUs < 500000 ||
            (record.lastReportUs != 0 && now - record.lastReportUs < 1000000)) return false;
        record.lastReportUs = now;
        out = record;
        return true;
    }

    void acquired(uintptr_t taskId, const char* task, const char* site, uint64_t now) {
        if (owner.depth == 0) {
            owner.taskId = taskId;
            copyTask(owner.task, task);
            owner.site = site;
            owner.sinceUs = now;
        }
        ++owner.depth;
    }

    void releasing(uintptr_t taskId, uint64_t now) {
        if (owner.depth == 0 || owner.taskId != taskId) return;
        if (--owner.depth == 0) {
            const uint64_t held = now - owner.sinceUs;
            if (held > maxHoldUs) maxHoldUs = held;
            owner = Owner();
        }
    }

    void heartbeat(uint64_t now) {
        if (lastLoopUs != 0 && now - lastLoopUs > maxLoopGapUs) maxLoopGapUs = now - lastLoopUs;
        lastLoopUs = now;
    }

    Owner owner {};
    uint64_t maxHoldUs {};
    uint64_t lastLoopUs {};
    uint64_t maxLoopGapUs {};
    uint32_t droppedActive {};
    uint32_t droppedCompleted {};

private:
    Record active_[Active] {};
    Record completed_[Completed] {};
    size_t head_ {};
    size_t count_ {};
    uint32_t nextId_ {};

    Record* find(uint32_t id) {
        if (id == 0) return nullptr;
        for (Record& record : active_) if (record.id == id) return &record;
        return nullptr;
    }

    static void copyTask(char (&out)[16], const char* task) {
        if (task) std::strncpy(out, task, sizeof(out) - 1);
        out[sizeof(out) - 1] = '\0';
    }
};

} // namespace io_probe
} // namespace core
