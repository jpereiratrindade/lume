#include "lume/store.hpp"

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
    throw std::runtime_error("invalid hexadecimal data in state file");
}

std::string hex_decode(std::string_view input) {
    if (input.size() % 2 != 0) throw std::runtime_error("truncated data in state file");
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
        throw std::runtime_error("invalid number in state file");
    }
    return value;
}

std::int64_t epoch(TimePoint value) {
    return value.time_since_epoch().count();
}

TimePoint time_from_epoch(const std::string& value) {
    return TimePoint{std::chrono::seconds{number<std::int64_t>(value)}};
}

}  // namespace

std::string to_string(IntentionStatus status) {
    switch (status) {
        case IntentionStatus::open: return "open";
        case IntentionStatus::active: return "active";
        case IntentionStatus::completed: return "completed";
        case IntentionStatus::dismissed: return "dismissed";
    }
    throw std::runtime_error("unknown intention status");
}

std::optional<IntentionStatus> intention_status_from_string(const std::string& value) {
    if (value == "open") return IntentionStatus::open;
    if (value == "active") return IntentionStatus::active;
    if (value == "completed") return IntentionStatus::completed;
    if (value == "dismissed") return IntentionStatus::dismissed;
    return std::nullopt;
}

std::string format_time(TimePoint time) {
    const std::time_t raw = Clock::to_time_t(time);
    std::tm local{};
    localtime_r(&raw, &local);
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%dT%H:%M:%S%z");
    return output.str();
}

std::optional<TimePoint> parse_time(const std::string& value) {
    std::tm local{};
    std::istringstream input(value);
    input >> std::get_time(&local, "%Y-%m-%dT%H:%M:%S");
    if (input.fail()) {
        input.clear();
        input.str(value);
        input >> std::get_time(&local, "%Y-%m-%dT%H:%M");
    }
    if (input.fail()) return std::nullopt;
    local.tm_isdst = -1;
    return TimePoint{std::chrono::seconds{std::mktime(&local)}};
}

Store::Store(std::filesystem::path path) : path_(std::move(path)) {}

const std::filesystem::path& Store::path() const noexcept { return path_; }

State Store::load() const {
    State state;
    if (!std::filesystem::exists(path_)) return state;
    if (std::filesystem::is_regular_file(path_) && std::filesystem::file_size(path_) == 0) return state;

    std::ifstream input(path_);
    if (!input) throw std::runtime_error("could not open state file: " + path_.string());

    std::string line;
    if (!std::getline(input, line) || line != "LUME\t1") {
        throw std::runtime_error("unsupported or damaged Lume state file");
    }

    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) continue;
        const auto fields = split_tabs(line);
        try {
            if (fields[0] == "N" && fields.size() == 2) {
                state.next_id = number<std::uint64_t>(fields[1]);
            } else if (fields[0] == "E" && fields.size() == 4) {
                state.expressions.push_back(Expression{
                    number<std::uint64_t>(fields[1]), time_from_epoch(fields[2]), hex_decode(fields[3])});
            } else if (fields[0] == "I" && fields.size() == 11) {
                const auto status = intention_status_from_string(fields[7]);
                if (!status) throw std::runtime_error("invalid intention status");
                Intention intention{
                    .id = number<std::uint64_t>(fields[1]),
                    .expression_id = number<std::uint64_t>(fields[2]),
                    .subject = hex_decode(fields[3]),
                    .window_start = time_from_epoch(fields[4]),
                    .window_end = time_from_epoch(fields[5]),
                    .precision = hex_decode(fields[6]),
                    .authority = hex_decode(fields[8]),
                    .interpretation_source = hex_decode(fields[9]),
                    .status = *status,
                    .last_interaction_at = std::nullopt,
                };
                if (fields[10] != "-") intention.last_interaction_at = time_from_epoch(fields[10]);
                state.intentions.push_back(std::move(intention));
            } else if (fields[0] == "X" && fields.size() == 9) {
                state.interactions.push_back(Interaction{
                    number<std::uint64_t>(fields[1]), number<std::uint64_t>(fields[2]),
                    time_from_epoch(fields[3]), hex_decode(fields[4]), hex_decode(fields[5]),
                    hex_decode(fields[6]), hex_decode(fields[7])});
            } else {
                throw std::runtime_error("unknown record");
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("invalid state at line " + std::to_string(line_number) +
                                     ": " + error.what());
        }
    }
    return state;
}

void Store::save(const State& state) const {
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
    auto temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) throw std::runtime_error("could not write state file: " + temporary.string());
        output << "LUME\t1\nN\t" << state.next_id << '\n';
        for (const auto& expression : state.expressions) {
            output << "E\t" << expression.id << '\t' << epoch(expression.recorded_at) << '\t'
                   << hex_encode(expression.text) << '\n';
        }
        for (const auto& intention : state.intentions) {
            output << "I\t" << intention.id << '\t' << intention.expression_id << '\t'
                   << hex_encode(intention.subject) << '\t' << epoch(intention.window_start) << '\t'
                   << epoch(intention.window_end) << '\t' << hex_encode(intention.precision) << '\t'
                   << to_string(intention.status) << '\t' << hex_encode(intention.authority) << '\t'
                   << hex_encode(intention.interpretation_source) << '\t';
            if (intention.last_interaction_at) output << epoch(*intention.last_interaction_at);
            else output << '-';
            output << '\n';
        }
        for (const auto& interaction : state.interactions) {
            output << "X\t" << interaction.id << '\t' << interaction.intention_id << '\t'
                   << epoch(interaction.created_at) << '\t' << hex_encode(interaction.message) << '\t'
                   << hex_encode(interaction.reason) << '\t' << hex_encode(interaction.decision)
                   << '\t' << hex_encode(interaction.formulation_source) << "\t-\n";
        }
        output.flush();
        if (!output) throw std::runtime_error("could not finish writing state file");
    }
    std::filesystem::rename(temporary, path_);
}

}  // namespace lume
