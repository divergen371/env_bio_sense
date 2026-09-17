#include "services/io_probe.h"
#include "services/logger.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>

namespace services {
namespace {
using core::io_probe::Record;
using core::io_probe::Owner;
core::io_probe::State<> state;
portMUX_TYPE probeMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t reporterHandle = nullptr;

const char* stageName(IoProbe::Stage stage) {
    static const char* const names[] = {
        "enter", "wait", "open", "scan", "close", "snapshot", "filter", "sort", "json", "send",
        "prepare", "wal_read", "tx_start", "inspect", "read", "write", "flush", "reopen",
        "verify", "consume", "tx_finish", "mount"
    };
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(IoProbe::Stage::Count), "stage names");
    return names[static_cast<size_t>(stage)];
}

// Reporter is the only path which logs. Measured tasks only touch bounded RAM
// under a separate, short critical section; no SD/FRAM/Storage mutex is taken.
void printComplete(const Record& r) {
    Logger::info("IoProbe", "done id=%lu op=%s task=%s cpu=%d pri=%lu start_us=%llu total_us=%llu result=%s",
        static_cast<unsigned long>(r.id), r.operation, r.task, r.core,
        static_cast<unsigned long>(r.priority), r.startedUs, r.endedUs - r.startedUs, r.outcome);
    char line[210] {};
    size_t used = 0;
    for (size_t i = 0; i < static_cast<size_t>(IoProbe::Stage::Count); ++i) {
        if (r.stageUs[i] == 0) continue;
        if (used > 145) {
            Logger::info("IoProbe", "time_us id=%lu %s", static_cast<unsigned long>(r.id), line);
            used = 0;
            line[0] = '\0';
        }
        const int written = std::snprintf(line + used, sizeof(line) - used, "%s=%llu ",
            stageName(static_cast<IoProbe::Stage>(i)), r.stageUs[i]);
        if (written > 0) used += static_cast<size_t>(written);
    }
    if (used) Logger::info("IoProbe", "time_us id=%lu %s", static_cast<unsigned long>(r.id), line);
    Logger::info("IoProbe", "counts id=%lu entries=%lu dirs=%lu files=%lu bytes=%lu records=%lu heap_sample_min=%lu",
        static_cast<unsigned long>(r.id), static_cast<unsigned long>(r.metrics[0]),
        static_cast<unsigned long>(r.metrics[1]), static_cast<unsigned long>(r.metrics[2]),
        static_cast<unsigned long>(r.metrics[3]), static_cast<unsigned long>(r.metrics[4]),
        static_cast<unsigned long>(r.minHeap == UINT32_MAX ? 0 : r.minHeap));
}

void reporter(void*) {
    uint64_t lastSummaryUs = 0;
    while (true) {
        uint64_t now = esp_timer_get_time();
        Record record;
        Owner owner;
        for (size_t i = 0; i < 12; ++i) {
            portENTER_CRITICAL(&probeMux);
            now = esp_timer_get_time();
            const bool stalled = state.stalled(i, now, record);
            owner = state.owner;
            portEXIT_CRITICAL(&probeMux);
            if (!stalled) continue;
            Logger::warn("IoProbe", "active id=%lu op=%s task=%s stage=%s age_us=%llu stage_us=%llu entries=%lu records=%lu",
                static_cast<unsigned long>(record.id), record.operation, record.task, stageName(record.stage),
                now - record.startedUs, now - record.stageSinceUs,
                static_cast<unsigned long>(record.metrics[0]), static_cast<unsigned long>(record.metrics[4]));
            if (owner.depth) Logger::warn("IoProbe", "owner task=%s site=%s depth=%lu held_us=%llu",
                owner.task, owner.site, static_cast<unsigned long>(owner.depth), now - owner.sinceUs);
        }
        // Bound serial work and expose overflow instead of delaying producers.
        for (unsigned i = 0; i < 4; ++i) {
            portENTER_CRITICAL(&probeMux);
            const bool available = state.pop(record);
            portEXIT_CRITICAL(&probeMux);
            if (!available) break;
            printComplete(record);
        }
        if (now - lastSummaryUs >= 5000000) {
            lastSummaryUs = now;
            portENTER_CRITICAL(&probeMux);
            now = esp_timer_get_time();
            const uint64_t hold = state.maxHoldUs;
            const uint64_t loopGap = state.maxLoopGapUs;
            const uint64_t loopAge = state.lastLoopUs ? now - state.lastLoopUs : 0;
            const uint32_t lostActive = state.droppedActive;
            const uint32_t lostCompleted = state.droppedCompleted;
            portEXIT_CRITICAL(&probeMux);
            Logger::info("IoProbe", "health hold_max_us=%llu loop_gap_max_us=%llu loop_age_us=%llu lost_active=%lu lost_done=%lu heap_low=%u stack_free=%u",
                hold, loopGap, loopAge, static_cast<unsigned long>(lostActive), static_cast<unsigned long>(lostCompleted),
                ESP.getMinFreeHeap(), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
} // namespace

IoProbe::IoProbe(const char* operation, uint64_t thresholdUs) {
    const uint64_t now = esp_timer_get_time();
    const char* task = pcTaskGetName(nullptr);
    const uint32_t priority = uxTaskPriorityGet(nullptr);
    const int cpu = xPortGetCoreID();
    portENTER_CRITICAL(&probeMux);
    id_ = state.begin(now, operation, task, priority, cpu, thresholdUs);
    portEXIT_CRITICAL(&probeMux);
}

IoProbe::~IoProbe() { finish(); }

void IoProbe::stage(Stage stage) {
    const uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&probeMux);
    state.stage(id_, stage, now);
    portEXIT_CRITICAL(&probeMux);
}

void IoProbe::add(Metric metric, uint32_t delta) {
    portENTER_CRITICAL(&probeMux);
    state.metric(id_, metric, delta);
    portEXIT_CRITICAL(&probeMux);
}

void IoProbe::sampleHeap() {
    const uint32_t free = ESP.getFreeHeap();
    portENTER_CRITICAL(&probeMux);
    state.heap(id_, free);
    portEXIT_CRITICAL(&probeMux);
}

void IoProbe::finish(const char* outcome) {
    if (!id_) return;
    const uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&probeMux);
    state.end(id_, now, outcome);
    portEXIT_CRITICAL(&probeMux);
    id_ = 0;
}

bool IoProbe::beginReporter() {
    if (reporterHandle) return true;
    char hash[65] {};
    esp_ota_get_app_elf_sha256(hash, sizeof(hash));
    Logger::info("IoProbe", "R01/R02 enabled; ELF=%s; RAM-only, no persistence", hash);
    const bool ok = xTaskCreatePinnedToCore(reporter, "IoProbeTask", 4096, nullptr, 1, &reporterHandle, 0) == pdPASS;
    if (!ok) Logger::error("IoProbe", "Reporter task creation failed; diagnostic evidence unavailable");
    return ok;
}

void IoProbe::storageAcquired(const char* site) {
    const uintptr_t task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    const char* name = pcTaskGetName(nullptr);
    const uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&probeMux);
    state.acquired(task, name, site, now);
    portEXIT_CRITICAL(&probeMux);
}

void IoProbe::storageReleasing() {
    const uintptr_t task = reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
    const uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&probeMux);
    state.releasing(task, now);
    portEXIT_CRITICAL(&probeMux);
}

void IoProbe::loopHeartbeat() {
    const uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&probeMux);
    state.heartbeat(now);
    portEXIT_CRITICAL(&probeMux);
}

} // namespace services
