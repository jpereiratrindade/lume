#include "lume/ledger.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace lume {
namespace {

std::string hex_encode(std::string_view input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    return output;
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    throw std::runtime_error("invalid hexadecimal data in ledger file");
}

std::string hex_decode(std::string_view input) {
    if (input.size() % 2 != 0) throw std::runtime_error("truncated data in ledger file");
    std::string output;
    output.reserve(input.size() / 2);
    for (std::size_t index = 0; index < input.size(); index += 2) {
        output.push_back(static_cast<char>((hex_value(input[index]) << 4) |
                                           hex_value(input[index + 1])));
    }
    return output;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const auto end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return fields;
}

template <typename Integer>
Integer number(std::string_view text) {
    Integer value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || pointer != text.data() + text.size()) {
        throw std::runtime_error("invalid number in ledger file: " + std::string(text));
    }
    return value;
}

std::int64_t epoch(TimePoint value) {
    return value.time_since_epoch().count();
}

TimePoint time_from_epoch(const std::string& value) {
    return TimePoint{std::chrono::seconds{number<std::int64_t>(value)}};
}

std::string encode_blocks(const std::vector<PlanBlock>& blocks) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (i > 0) ss << ";";
        ss << hex_encode(blocks[i].title) << ","
           << epoch(blocks[i].start) << ","
           << epoch(blocks[i].end) << ","
           << hex_encode(blocks[i].category) << ","
           << blocks[i].intention_id;
    }
    return ss.str();
}

std::vector<PlanBlock> decode_blocks(const std::string& encoded) {
    std::vector<PlanBlock> blocks;
    if (encoded.empty()) return blocks;
    std::istringstream ss(encoded);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.empty()) continue;
        std::istringstream item_ss(item);
        std::string title_hex, start_str, end_str, cat_hex, int_id_str;
        if (std::getline(item_ss, title_hex, ',') &&
            std::getline(item_ss, start_str, ',') &&
            std::getline(item_ss, end_str, ',') &&
            std::getline(item_ss, cat_hex, ',') &&
            std::getline(item_ss, int_id_str, ',')) {
            blocks.push_back(PlanBlock{
                .title = hex_decode(title_hex),
                .start = time_from_epoch(start_str),
                .end = time_from_epoch(end_str),
                .category = hex_decode(cat_hex),
                .intention_id = number<std::uint64_t>(int_id_str),
            });
        }
    }
    return blocks;
}

std::string encode_strings(const std::vector<std::string>& list) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i > 0) ss << ";";
        ss << hex_encode(list[i]);
    }
    return ss.str();
}

std::vector<std::string> decode_strings(const std::string& encoded) {
    std::vector<std::string> list;
    if (encoded.empty()) return list;
    std::istringstream ss(encoded);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (!item.empty()) list.push_back(hex_decode(item));
    }
    return list;
}

}  // namespace

Ledger::Ledger(std::filesystem::path path) : path_(std::move(path)) {}

const std::filesystem::path& Ledger::path() const noexcept { return path_; }

void Ledger::append(const EventRecord& record) {
    append_batch({record});
}

void Ledger::append_batch(const std::vector<EventRecord>& records) {
    if (records.empty()) return;
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());

    bool is_new_file = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

    std::ofstream output(path_, std::ios::app);
    if (!output) throw std::runtime_error("could not open ledger for writing: " + path_.string());

    if (is_new_file) {
        output << "LUME-LEDGER\t1\n";
    }

    for (const auto& record : records) {
        output << record.sequence_number << '\t'
               << epoch(record.recorded_at) << '\t'
               << hex_encode(record.authority) << '\t';

        std::visit([&](const auto& event) {
            using T = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<T, EventExpressionRecorded>) {
                output << "EXPR\t" << event.id << '\t' << hex_encode(event.text);
            } else if constexpr (std::is_same_v<T, EventIntentionDerived>) {
                output << "INT_DERIVED\t" << event.id << '\t' << event.expression_id << '\t'
                       << hex_encode(event.subject) << '\t' << epoch(event.window_start) << '\t'
                       << epoch(event.window_end) << '\t' << hex_encode(event.precision) << '\t'
                       << hex_encode(event.authority) << '\t' << hex_encode(event.interpretation_source);
            } else if constexpr (std::is_same_v<T, EventIntentionStatusChanged>) {
                output << "INT_STATUS\t" << event.intention_id << '\t'
                       << to_string(event.new_status) << '\t' << hex_encode(event.reason);
            } else if constexpr (std::is_same_v<T, EventIntentionDeferred>) {
                output << "INT_DEFERRED\t" << event.intention_id << '\t'
                       << epoch(event.new_start) << '\t' << epoch(event.new_end) << '\t'
                       << hex_encode(event.precision) << '\t' << hex_encode(event.reason);
            } else if constexpr (std::is_same_v<T, EventInteractionRecorded>) {
                output << "INTERACTION\t" << event.id << '\t' << event.intention_id << '\t'
                       << hex_encode(event.message) << '\t' << hex_encode(event.reason) << '\t'
                       << hex_encode(event.decision) << '\t' << hex_encode(event.formulation_source);
            } else if constexpr (std::is_same_v<T, EventPlanProposed>) {
                output << "PLAN_PROP\t" << event.id << '\t'
                       << hex_encode(event.horizon) << '\t' << hex_encode(event.summary) << '\t'
                       << hex_encode(encode_blocks(event.blocks)) << '\t'
                       << hex_encode(encode_strings(event.points_of_attention)) << '\t'
                       << event.status << '\t' << hex_encode(event.source);
            } else if constexpr (std::is_same_v<T, EventPlanApplied>) {
                output << "PLAN_APPL\t" << event.plan_id << '\t' << hex_encode(event.reason);
            } else if constexpr (std::is_same_v<T, EventPlanDiscarded>) {
                output << "PLAN_DISC\t" << event.plan_id << '\t' << hex_encode(event.reason);
            } else if constexpr (std::is_same_v<T, EventAutomationProposed>) {
                output << "AUTO_PROP\t" << event.id << '\t'
                       << hex_encode(event.trigger_when) << '\t' << hex_encode(event.condition_if) << '\t'
                       << hex_encode(event.action_then) << '\t' << hex_encode(event.authority) << '\t'
                       << event.status;
            } else if constexpr (std::is_same_v<T, EventAutomationTriggered>) {
                output << "AUTO_TRIG\t" << event.automation_id << '\t' << hex_encode(event.explanation);
            }
        }, record.payload);

        output << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("failed to write records to ledger");
}

std::vector<EventRecord> Ledger::read_all() const {
    std::vector<EventRecord> records;
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0) return records;

    std::ifstream input(path_);
    if (!input) throw std::runtime_error("could not read ledger file: " + path_.string());

    std::string header;
    if (!std::getline(input, header)) return records;

    // Handle legacy snapshot state file transparently
    if (header == "LUME\t1") {
        return records;
    }

    if (header != "LUME-LEDGER\t1") {
        throw std::runtime_error("unsupported ledger header: " + header);
    }

    std::string line;
    std::size_t line_no = 1;
    while (std::getline(input, line)) {
        ++line_no;
        if (line.empty()) continue;
        const auto f = split_tabs(line);
        if (f.size() < 4) throw std::runtime_error("malformed ledger record at line " + std::to_string(line_no));

        EventRecord record{
            .sequence_number = number<std::uint64_t>(f[0]),
            .recorded_at = time_from_epoch(f[1]),
            .authority = hex_decode(f[2]),
            .payload = EventExpressionRecorded{},
        };

        const std::string& type = f[3];
        if (type == "EXPR" && f.size() >= 6) {
            record.payload = EventExpressionRecorded{
                .id = number<std::uint64_t>(f[4]),
                .timestamp = record.recorded_at,
                .text = hex_decode(f[5]),
            };
        } else if (type == "INT_DERIVED" && f.size() >= 12) {
            record.payload = EventIntentionDerived{
                .id = number<std::uint64_t>(f[4]),
                .expression_id = number<std::uint64_t>(f[5]),
                .subject = hex_decode(f[6]),
                .window_start = time_from_epoch(f[7]),
                .window_end = time_from_epoch(f[8]),
                .precision = hex_decode(f[9]),
                .authority = hex_decode(f[10]),
                .interpretation_source = hex_decode(f[11]),
            };
        } else if (type == "INT_STATUS" && f.size() >= 6) {
            const auto st = intention_status_from_string(f[5]);
            record.payload = EventIntentionStatusChanged{
                .intention_id = number<std::uint64_t>(f[4]),
                .new_status = st.value_or(IntentionStatus::open),
                .reason = hex_decode(f[6]),
                .at = record.recorded_at,
            };
        } else if (type == "INT_DEFERRED" && f.size() >= 8) {
            record.payload = EventIntentionDeferred{
                .intention_id = number<std::uint64_t>(f[4]),
                .new_start = time_from_epoch(f[5]),
                .new_end = time_from_epoch(f[6]),
                .precision = hex_decode(f[7]),
                .reason = hex_decode(f[8]),
                .at = record.recorded_at,
            };
        } else if (type == "INTERACTION" && f.size() >= 10) {
            record.payload = EventInteractionRecorded{
                .id = number<std::uint64_t>(f[4]),
                .intention_id = number<std::uint64_t>(f[5]),
                .timestamp = record.recorded_at,
                .message = hex_decode(f[6]),
                .reason = hex_decode(f[7]),
                .decision = hex_decode(f[8]),
                .formulation_source = hex_decode(f[9]),
            };
        } else if (type == "PLAN_PROP" && f.size() >= 10) {
            record.payload = EventPlanProposed{
                .id = number<std::uint64_t>(f[4]),
                .created_at = record.recorded_at,
                .horizon = hex_decode(f[5]),
                .summary = hex_decode(f[6]),
                .blocks = decode_blocks(hex_decode(f[7])),
                .points_of_attention = decode_strings(hex_decode(f[8])),
                .status = f[9],
                .source = f.size() >= 11 ? hex_decode(f[10]) : "",
            };
        } else if (type == "PLAN_APPL" && f.size() >= 6) {
            record.payload = EventPlanApplied{
                .plan_id = number<std::uint64_t>(f[4]),
                .applied_at = record.recorded_at,
                .reason = hex_decode(f[5]),
            };
        } else if (type == "PLAN_DISC" && f.size() >= 6) {
            record.payload = EventPlanDiscarded{
                .plan_id = number<std::uint64_t>(f[4]),
                .discarded_at = record.recorded_at,
                .reason = hex_decode(f[5]),
            };
        } else if (type == "AUTO_PROP" && f.size() >= 10) {
            record.payload = EventAutomationProposed{
                .id = number<std::uint64_t>(f[4]),
                .created_at = record.recorded_at,
                .trigger_when = hex_decode(f[5]),
                .condition_if = hex_decode(f[6]),
                .action_then = hex_decode(f[7]),
                .authority = hex_decode(f[8]),
                .status = f[9],
            };
        } else if (type == "AUTO_TRIG" && f.size() >= 6) {
            record.payload = EventAutomationTriggered{
                .automation_id = number<std::uint64_t>(f[4]),
                .triggered_at = record.recorded_at,
                .explanation = hex_decode(f[5]),
            };
        }
        records.push_back(std::move(record));
    }
    return records;
}

State Ledger::project_state() const {
    State state;
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0) return state;

    // Check if it's an old legacy snapshot file
    std::ifstream check_input(path_);
    std::string header;
    if (std::getline(check_input, header) && header == "LUME\t1") {
        // Fallback loader for legacy snapshots
        check_input.clear();
        check_input.seekg(0);
        std::string line;
        std::getline(check_input, line); // Skip LUME\t1
        while (std::getline(check_input, line)) {
            if (line.empty()) continue;
            const auto fields = split_tabs(line);
            if (fields[0] == "N" && fields.size() == 2) {
                state.next_id = number<std::uint64_t>(fields[1]);
            } else if (fields[0] == "E" && fields.size() == 4) {
                state.expressions.push_back(Expression{
                    number<std::uint64_t>(fields[1]), time_from_epoch(fields[2]), hex_decode(fields[3])});
            } else if (fields[0] == "I" && fields.size() == 11) {
                const auto status = intention_status_from_string(fields[7]);
                Intention intention{
                    .id = number<std::uint64_t>(fields[1]),
                    .expression_id = number<std::uint64_t>(fields[2]),
                    .subject = hex_decode(fields[3]),
                    .window_start = time_from_epoch(fields[4]),
                    .window_end = time_from_epoch(fields[5]),
                    .precision = hex_decode(fields[6]),
                    .authority = hex_decode(fields[8]),
                    .interpretation_source = hex_decode(fields[9]),
                    .status = status.value_or(IntentionStatus::open),
                    .last_interaction_at = std::nullopt,
                };
                if (fields[10] != "-") intention.last_interaction_at = time_from_epoch(fields[10]);
                state.intentions.push_back(std::move(intention));
            } else if (fields[0] == "X" && fields.size() == 9) {
                state.interactions.push_back(Interaction{
                    number<std::uint64_t>(fields[1]), number<std::uint64_t>(fields[2]),
                    time_from_epoch(fields[3]), hex_decode(fields[4]), hex_decode(fields[5]),
                    hex_decode(fields[6]), hex_decode(fields[7])});
            }
        }
        return state;
    }

    const auto records = read_all();
    std::uint64_t max_id = 0;

    for (const auto& record : records) {
        std::visit([&](const auto& event) {
            using T = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<T, EventExpressionRecorded>) {
                max_id = std::max(max_id, event.id);
                state.expressions.push_back(Expression{
                    .id = event.id,
                    .recorded_at = event.timestamp,
                    .text = event.text,
                });
            } else if constexpr (std::is_same_v<T, EventIntentionDerived>) {
                max_id = std::max(max_id, event.id);
                state.intentions.push_back(Intention{
                    .id = event.id,
                    .expression_id = event.expression_id,
                    .subject = event.subject,
                    .window_start = event.window_start,
                    .window_end = event.window_end,
                    .precision = event.precision,
                    .authority = event.authority,
                    .interpretation_source = event.interpretation_source,
                    .status = IntentionStatus::open,
                    .last_interaction_at = std::nullopt,
                });
            } else if constexpr (std::is_same_v<T, EventIntentionStatusChanged>) {
                auto it = std::find_if(state.intentions.begin(), state.intentions.end(),
                                       [&](const auto& item) { return item.id == event.intention_id; });
                if (it != state.intentions.end()) {
                    it->status = event.new_status;
                }
            } else if constexpr (std::is_same_v<T, EventIntentionDeferred>) {
                auto it = std::find_if(state.intentions.begin(), state.intentions.end(),
                                       [&](const auto& item) { return item.id == event.intention_id; });
                if (it != state.intentions.end()) {
                    it->window_start = event.new_start;
                    it->window_end = event.new_end;
                    it->precision = event.precision;
                    it->last_interaction_at.reset();
                }
            } else if constexpr (std::is_same_v<T, EventInteractionRecorded>) {
                max_id = std::max(max_id, event.id);
                auto it = std::find_if(state.intentions.begin(), state.intentions.end(),
                                       [&](const auto& item) { return item.id == event.intention_id; });
                if (it != state.intentions.end()) {
                    it->last_interaction_at = event.timestamp;
                }
                state.interactions.push_back(Interaction{
                    .id = event.id,
                    .intention_id = event.intention_id,
                    .created_at = event.timestamp,
                    .message = event.message,
                    .reason = event.reason,
                    .decision = event.decision,
                    .formulation_source = event.formulation_source,
                });
            } else if constexpr (std::is_same_v<T, EventPlanProposed>) {
                max_id = std::max(max_id, event.id);
                state.plan_proposals.push_back(PlanProposal{
                    .id = event.id,
                    .created_at = event.created_at,
                    .horizon = event.horizon,
                    .summary = event.summary,
                    .blocks = event.blocks,
                    .points_of_attention = event.points_of_attention,
                    .status = event.status,
                    .source = event.source,
                });
            } else if constexpr (std::is_same_v<T, EventPlanApplied>) {
                auto it = std::find_if(state.plan_proposals.begin(), state.plan_proposals.end(),
                                       [&](const auto& p) { return p.id == event.plan_id; });
                if (it != state.plan_proposals.end()) {
                    it->status = "applied";
                }
            } else if constexpr (std::is_same_v<T, EventPlanDiscarded>) {
                auto it = std::find_if(state.plan_proposals.begin(), state.plan_proposals.end(),
                                       [&](const auto& p) { return p.id == event.plan_id; });
                if (it != state.plan_proposals.end()) {
                    it->status = "discarded";
                }
            } else if constexpr (std::is_same_v<T, EventAutomationProposed>) {
                max_id = std::max(max_id, event.id);
                state.automation_proposals.push_back(AutomationProposal{
                    .id = event.id,
                    .created_at = event.created_at,
                    .trigger_when = event.trigger_when,
                    .condition_if = event.condition_if,
                    .action_then = event.action_then,
                    .authority = event.authority,
                    .status = event.status,
                });
            } else if constexpr (std::is_same_v<T, EventAutomationTriggered>) {
                // Keep record of triggered automations
            }
        }, record.payload);
    }

    state.next_id = max_id + 1;
    return state;
}

}  // namespace lume
