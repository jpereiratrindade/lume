#pragma once

#include "lume/domain.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lume {

// --- Canonical Event Payloads ---

struct EventExpressionRecorded {
    std::uint64_t id{};
    TimePoint timestamp{};
    std::string text;
};

struct EventIntentionDerived {
    std::uint64_t id{};
    std::uint64_t expression_id{};
    std::string subject;
    TimePoint window_start{};
    TimePoint window_end{};
    std::string precision;
    std::string authority{"user"};
    std::string interpretation_source;
};

struct EventIntentionStatusChanged {
    std::uint64_t intention_id{};
    IntentionStatus new_status{IntentionStatus::open};
    std::string reason;
    TimePoint at{};
};

struct EventIntentionDeferred {
    std::uint64_t intention_id{};
    TimePoint new_start{};
    TimePoint new_end{};
    std::string precision;
    std::string reason;
    TimePoint at{};
};

struct EventInteractionRecorded {
    std::uint64_t id{};
    std::uint64_t intention_id{};
    TimePoint timestamp{};
    std::string message;
    std::string reason;
    std::string decision{"INTERACT"};
    std::string formulation_source;
};

struct EventPlanProposed {
    std::uint64_t id{};
    TimePoint created_at{};
    std::string horizon;
    std::string summary;
    std::vector<PlanBlock> blocks;
    std::vector<std::string> points_of_attention;
    std::string status{"draft"}; // draft, applied, discarded
    std::string source;
};

struct EventPlanApplied {
    std::uint64_t plan_id{};
    TimePoint applied_at{};
    std::string reason;
};

struct EventPlanDiscarded {
    std::uint64_t plan_id{};
    TimePoint discarded_at{};
    std::string reason;
};

struct EventAutomationCreated {
    std::uint64_t id{};
    TimePoint created_at{};
    std::string title;
    std::string trigger_when;
    std::string condition_if;
    std::string action_then;
    std::string authority{"suggest_only"};
    std::string status{"active"}; // "active", "paused", "discarded"
};

struct EventAutomationStatusChanged {
    std::uint64_t automation_id{};
    std::string new_status;
    std::string reason;
    TimePoint at{};
};

struct EventAutomationTriggered {
    std::uint64_t automation_id{};
    TimePoint triggered_at{};
    std::string explanation;
};

struct EventNotificationEmitted {
    std::uint64_t id{};
    std::uint64_t automation_id{};
    TimePoint emitted_at{};
    std::string title;
    std::string message;
    std::string action_type{"info"};
    std::optional<std::uint64_t> reference_id;
};

using EventPayload = std::variant<
    EventExpressionRecorded,
    EventIntentionDerived,
    EventIntentionStatusChanged,
    EventIntentionDeferred,
    EventInteractionRecorded,
    EventPlanProposed,
    EventPlanApplied,
    EventPlanDiscarded,
    EventAutomationCreated,
    EventAutomationStatusChanged,
    EventAutomationTriggered,
    EventNotificationEmitted
>;

struct EventRecord {
    std::uint64_t sequence_number{0}; // Assigned automatically by Ledger if 0
    TimePoint recorded_at{};
    std::string authority{"user"}; // "user", "core", "inferred"
    EventPayload payload;
};

class FileLockGuard {
public:
    explicit FileLockGuard(std::string lock_path = "");
    ~FileLockGuard();
    FileLockGuard(FileLockGuard&& other) noexcept;
    FileLockGuard& operator=(FileLockGuard&& other) noexcept;
    FileLockGuard(const FileLockGuard&) = delete;
    FileLockGuard& operator=(const FileLockGuard&) = delete;

private:
    std::string lock_path_;
};

class Ledger {
public:
    explicit Ledger(std::filesystem::path path);

    // Atomically appends records assigning monotonic sequence numbers
    void append(const EventRecord& record);
    void append_batch(std::vector<EventRecord> records);

    // Reads all events reconstructing the state
    [[nodiscard]] std::vector<EventRecord> read_all() const;
    [[nodiscard]] State project_state() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    // Migrates legacy LUME\t1 snapshot file to LUME-LEDGER\t1 with .bak backup
    bool migrate_if_needed() const;

    // Executes a callback under an exclusive flock covering read->project->mutate->append
    template <typename Func>
    auto with_exclusive_lock(Func&& func) const -> decltype(func()) {
        FileLockGuard lock = acquire_file_lock();
        migrate_if_needed();
        return func();
    }

private:
    [[nodiscard]] FileLockGuard acquire_file_lock() const;

    std::filesystem::path path_;
};

}  // namespace lume

