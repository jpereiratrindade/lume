#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lume {

using Clock = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock, std::chrono::seconds>;

enum class IntentionStatus { open, active, completed, dismissed };

struct Expression {
    std::uint64_t id{};
    TimePoint recorded_at{};
    std::string text;
};

struct Intention {
    std::uint64_t id{};
    std::uint64_t expression_id{};
    std::string subject;
    TimePoint window_start{};
    TimePoint window_end{};
    std::string precision;
    std::string authority{"user"};
    std::string interpretation_source;
    IntentionStatus status{IntentionStatus::open};
    std::optional<TimePoint> last_interaction_at;
};

struct Interaction {
    std::uint64_t id{};
    std::uint64_t intention_id{};
    TimePoint created_at{};
    std::string message;
    std::string reason;
    std::string decision{"INTERACT"};
    std::string formulation_source;
};

struct PlanBlock {
    std::string title;
    TimePoint start{};
    TimePoint end{};
    std::string category{"focus"}; // focus, meeting, buffer, review
    std::uint64_t intention_id{0};
};

struct PlanProposal {
    std::uint64_t id{};
    TimePoint created_at{};
    std::string horizon; // "morning", "afternoon", "day", "week"
    std::string summary;
    std::vector<PlanBlock> blocks;
    std::vector<std::string> points_of_attention;
    std::string status{"draft"}; // draft, applied, discarded
    std::string source;
};

struct AutomationProposal {
    std::uint64_t id{};
    TimePoint created_at{};
    std::string title;
    std::string trigger_when;
    std::string condition_if;
    std::string action_then;
    std::string authority{"suggest_only"}; // "suggest_only", "prepare_proposal", "execute"
    std::string status{"active"}; // "active", "paused", "discarded"
    std::optional<TimePoint> last_triggered_at;
};

struct NotificationRecord {
    std::uint64_t id{};
    std::uint64_t automation_id{};
    TimePoint emitted_at{};
    std::string title;
    std::string message;
    std::string action_type{"info"}; // "info", "plan_proposal", "eod_prompt"
    std::optional<std::uint64_t> reference_id; // e.g. plan_proposal_id
};

struct State {
    std::uint64_t next_id{1};
    std::vector<Expression> expressions;
    std::vector<Intention> intentions;
    std::vector<Interaction> interactions;
    std::vector<PlanProposal> plan_proposals;
    std::vector<AutomationProposal> automation_proposals;
    std::vector<NotificationRecord> notifications;
};

struct Outcome {
    std::string decision{};
    std::string message{};
    std::string reason{};
    bool changed{false};
    std::string type{"text"}; // "text", "plan_proposal", "automation_proposal", "notifications"
    std::optional<PlanProposal> plan_proposal{std::nullopt};
    std::optional<AutomationProposal> automation_proposal{std::nullopt};
    std::vector<NotificationRecord> emitted_notifications{};
};

std::string to_string(IntentionStatus status);
std::optional<IntentionStatus> intention_status_from_string(const std::string& value);
std::string format_time(TimePoint time);
std::optional<TimePoint> parse_time(const std::string& value);

}  // namespace lume
