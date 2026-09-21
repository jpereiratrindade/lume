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

struct State {
    std::uint64_t next_id{1};
    std::vector<Expression> expressions;
    std::vector<Intention> intentions;
    std::vector<Interaction> interactions;
};

struct Outcome {
    std::string decision;
    std::string message;
    std::string reason;
    bool changed{};
};

std::string to_string(IntentionStatus status);
std::optional<IntentionStatus> intention_status_from_string(const std::string& value);
std::string format_time(TimePoint time);
std::optional<TimePoint> parse_time(const std::string& value);

}  // namespace lume
