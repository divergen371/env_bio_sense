#include <unity.h>
#include "core/io_probe_state.h"

using namespace core::io_probe;

void test_stage_durations_are_monotonic_and_accumulate() {
    State<2, 2> state;
    const uint64_t start = UINT64_C(0xffffffff) - 10;
    const uint32_t id = state.begin(start, "files", "async_tcp", 10, 1, 0);
    state.stage(id, Stage::Wait, start + 5);
    state.stage(id, Stage::Scan, start + 25);
    state.stage(id, Stage::Wait, start + 100);
    state.end(id, start + 110, "returned");
    Record record;
    TEST_ASSERT_TRUE(state.pop(record));
    TEST_ASSERT_EQUAL_UINT64(5, record.stageUs[static_cast<size_t>(Stage::Enter)]);
    TEST_ASSERT_EQUAL_UINT64(30, record.stageUs[static_cast<size_t>(Stage::Wait)]);
    TEST_ASSERT_EQUAL_UINT64(75, record.stageUs[static_cast<size_t>(Stage::Scan)]);
    TEST_ASSERT_EQUAL_UINT64(110, record.endedUs - record.startedUs);
    TEST_ASSERT_EQUAL_STRING("async_tcp", record.task);
}

void test_active_capacity_does_not_evict_or_wait() {
    State<1, 1> state;
    const uint32_t id = state.begin(0, "files", "async_tcp", 10, 1, 0);
    TEST_ASSERT_EQUAL_UINT32(0, state.begin(1, "append", "loopTask", 1, 1, 0));
    state.stage(0, Stage::Scan, 2);
    state.end(0, 3, "ignored");
    state.end(id, 10, "returned");
    Record record;
    TEST_ASSERT_TRUE(state.pop(record));
    TEST_ASSERT_EQUAL_STRING("files", record.operation);
    TEST_ASSERT_EQUAL_UINT32(1, state.droppedActive);
}

void test_completed_overflow_is_counted_and_recovers() {
    State<1, 1> state;
    state.end(state.begin(0, "first", "task", 1, 1, 0), 10, "done");
    state.end(state.begin(20, "second", "task", 1, 1, 0), 30, "done");
    TEST_ASSERT_EQUAL_UINT32(1, state.droppedCompleted);
    Record record;
    TEST_ASSERT_TRUE(state.pop(record));
    TEST_ASSERT_EQUAL_STRING("first", record.operation);
    TEST_ASSERT_FALSE(state.pop(record));
    state.end(state.begin(40, "third", "task", 1, 1, 0), 50, "done");
    TEST_ASSERT_TRUE(state.pop(record));
    TEST_ASSERT_EQUAL_STRING("third", record.operation);
}

void test_stall_reports_before_completion_and_is_rate_limited() {
    State<1, 1> state;
    const uint32_t id = state.begin(100, "files", "task", 10, 1, 0);
    state.stage(id, Stage::Wait, 200);
    Record record;
    TEST_ASSERT_FALSE(state.stalled(0, 500099, record));
    TEST_ASSERT_TRUE(state.stalled(0, 500100, record));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Stage::Wait), static_cast<uint8_t>(record.stage));
    TEST_ASSERT_FALSE(state.stalled(0, 1500099, record));
    TEST_ASSERT_TRUE(state.stalled(0, 1500100, record));
    state.end(id, 1500200, "done");
    TEST_ASSERT_FALSE(state.stalled(0, 2500100, record));
}

void test_recursive_owner_retains_outer_site_and_hold() {
    State<1, 1> state;
    state.acquired(11, "StorageTask", "flush", 100);
    state.acquired(11, "StorageTask", "getPendingCount", 200);
    TEST_ASSERT_EQUAL_UINT32(2, state.owner.depth);
    TEST_ASSERT_EQUAL_STRING("flush", state.owner.site);
    state.releasing(22, 250); // Invalid release cannot erase the owner.
    state.releasing(11, 300);
    TEST_ASSERT_EQUAL_UINT32(1, state.owner.depth);
    TEST_ASSERT_EQUAL_UINT64(0, state.maxHoldUs);
    state.releasing(11, 600);
    TEST_ASSERT_EQUAL_UINT64(500, state.maxHoldUs);
    TEST_ASSERT_EQUAL_UINT32(0, state.owner.depth);
    state.acquired(22, "async_tcp", "files", 601);
    TEST_ASSERT_EQUAL_STRING("files", state.owner.site);
}

void test_metrics_heap_and_short_operation_threshold() {
    State<2, 2> state;
    const uint32_t id = state.begin(100, "files", "01234567890123456789", 10, 0, 100);
    state.metric(id, Metric::Entries, 7);
    state.metric(id, Metric::Entries, 2);
    state.heap(id, 20000);
    state.heap(id, 19000);
    state.heap(id, 21000);
    state.end(id, 200, "done");
    state.end(state.begin(300, "fast", "task", 1, 0, 100), 399, "done");
    Record record;
    TEST_ASSERT_TRUE(state.pop(record));
    TEST_ASSERT_EQUAL_UINT32(9, record.metrics[0]);
    TEST_ASSERT_EQUAL_UINT32(19000, record.minHeap);
    TEST_ASSERT_EQUAL_STRING("012345678901234", record.task);
    TEST_ASSERT_FALSE(state.pop(record));
}

void test_loop_gap_tracks_completed_stall() {
    State<1, 1> state;
    state.heartbeat(100);
    state.heartbeat(120);
    state.heartbeat(30000120);
    state.heartbeat(30000130);
    TEST_ASSERT_EQUAL_UINT64(30000000, state.maxLoopGapUs);
    TEST_ASSERT_EQUAL_UINT64(30000130, state.lastLoopUs);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_stage_durations_are_monotonic_and_accumulate);
    RUN_TEST(test_active_capacity_does_not_evict_or_wait);
    RUN_TEST(test_completed_overflow_is_counted_and_recovers);
    RUN_TEST(test_stall_reports_before_completion_and_is_rate_limited);
    RUN_TEST(test_recursive_owner_retains_outer_site_and_hold);
    RUN_TEST(test_metrics_heap_and_short_operation_threshold);
    RUN_TEST(test_loop_gap_tracks_completed_stall);
    return UNITY_END();
}
