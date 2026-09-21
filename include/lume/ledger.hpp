#pragma once

#include "lume/domain.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
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

struct EventAutomationProposed {
    std::uint64_t id{};
    TimePoint created_at{};
    std::string trigger_when;
    std::string condition_if;
    std::string action_then;
    std::string authority;
    std::string status{"pending"}; // pending, active, paused
};

struct EventAutomationTriggered {
    std::uint64_t automation_id{};
    TimePoint triggered_at{};
    std::string explanation;
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
    EventAutomationProposed,
    EventAutomationTriggered
>;

struct EventRecord {
    std::uint64_t sequence_number{};
    TimePoint recorded_at{};
    std::string authority{"user"}; // "user", "core", "inferred"
    EventPayload payload;
};

class Ledger {
public:
    explicit Ledger(std::filesystem::path path);

    void append(const EventRecord& record);
    void append_batch(const std::vector<EventRecord>& records);

    [[nodiscard]] std::vector<EventRecord> read_all() const;
    [[nodiscard]] State project_state() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

}  // namespace lume
