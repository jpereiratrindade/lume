#include "lume/ledger.hpp"

#include <algorithm>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace lume {
namespace {

struct LockInfo {
    int fd{-1};
    std::thread::id owner_thread{};
    int recursion_count{0};
    int waiters{0};
};

std::mutex g_lock_mutex;
std::condition_variable g_lock_cv;
std::unordered_map<std::string, LockInfo> g_locks;

void secure_directory(const std::filesystem::path& dir) {
    if (dir.empty()) return;
    std::filesystem::create_directories(dir);
    chmod(dir.c_str(), S_IRWXU); // 0700
}

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
        } else {
            throw std::runtime_error("malformed plan block encoding in ledger");
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

FileLockGuard::FileLockGuard(std::string lock_path) : lock_path_(std::move(lock_path)) {
    if (lock_path_.empty()) return;

    const auto this_thread = std::this_thread::get_id();
    std::unique_lock<std::mutex> guard(g_lock_mutex);
    auto& info = g_locks[lock_path_];

    info.waiters++;
    g_lock_cv.wait(guard, [&]() {
        return info.recursion_count == 0 || info.owner_thread == this_thread;
    });
    info.waiters--;

    if (info.recursion_count == 0) {
        info.owner_thread = this_thread;
        int fd = open(lock_path_.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd < 0) {
            if (info.waiters == 0) g_locks.erase(lock_path_);
            g_lock_cv.notify_all();
            throw std::runtime_error("could not open lock file: " + lock_path_);
        }
        chmod(lock_path_.c_str(), S_IRUSR | S_IWUSR); // 0600
        if (flock(fd, LOCK_EX) != 0) {
            close(fd);
            if (info.waiters == 0) g_locks.erase(lock_path_);
            g_lock_cv.notify_all();
            throw std::runtime_error("could not acquire exclusive flock: " + lock_path_);
        }
        info.fd = fd;
    }
    info.recursion_count++;
}

void FileLockGuard::release() noexcept {
    if (lock_path_.empty()) return;

    std::unique_lock<std::mutex> guard(g_lock_mutex);
    auto it = g_locks.find(lock_path_);
    if (it != g_locks.end() && it->second.owner_thread == std::this_thread::get_id()) {
        it->second.recursion_count--;
        if (it->second.recursion_count <= 0) {
            if (it->second.fd >= 0) {
                flock(it->second.fd, LOCK_UN);
                close(it->second.fd);
                it->second.fd = -1;
            }
            it->second.recursion_count = 0;
            it->second.owner_thread = std::thread::id{};
            if (it->second.waiters == 0) {
                g_locks.erase(it);
            }
            g_lock_cv.notify_all();
        }
    }
    lock_path_.clear();
}

FileLockGuard::~FileLockGuard() {
    release();
}

FileLockGuard::FileLockGuard(FileLockGuard&& other) noexcept : lock_path_(std::move(other.lock_path_)) {
    other.lock_path_.clear();
}

FileLockGuard& FileLockGuard::operator=(FileLockGuard&& other) noexcept {
    if (this != &other) {
        release();
        lock_path_ = std::move(other.lock_path_);
        other.lock_path_.clear();
    }
    return *this;
}

Ledger::Ledger(std::filesystem::path path) : path_(std::move(path)) {}

const std::filesystem::path& Ledger::path() const noexcept { return path_; }

FileLockGuard Ledger::acquire_file_lock() const {
    if (path_.has_parent_path()) secure_directory(path_.parent_path());
    const auto lock_file_path = std::filesystem::absolute(path_).string() + ".lock";
    return FileLockGuard(lock_file_path);
}

bool Ledger::migrate_if_needed() const {
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0) return false;

    std::ifstream check_input(path_);
    if (!check_input) return false;
    std::string header;
    if (!std::getline(check_input, header) || header != "LUME\t1") return false;

    // Read legacy state
    std::vector<EventRecord> migrated_records;
    std::uint64_t seq = 1;
    std::string line;
    while (std::getline(check_input, line)) {
        if (line.empty()) continue;
        const auto f = split_tabs(line);
        if (f.empty()) continue;
        if (f[0] == "E" && f.size() == 4) {
            migrated_records.push_back(EventRecord{
                .sequence_number = seq++,
                .recorded_at = time_from_epoch(f[2]),
                .authority = "user",
                .epistemic_class = EpistemicClass::user_declared,
                .payload = EventExpressionRecorded{
                    .id = number<std::uint64_t>(f[1]),
                    .timestamp = time_from_epoch(f[2]),
                    .text = hex_decode(f[3]),
                },
            });
        } else if (f[0] == "I" && f.size() == 11) {
            const auto status = intention_status_from_string(f[7]);
            migrated_records.push_back(EventRecord{
                .sequence_number = seq++,
                .recorded_at = time_from_epoch(f[4]),
                .authority = hex_decode(f[8]),
                .epistemic_class = EpistemicClass::derived,
                .payload = EventIntentionDerived{
                    .id = number<std::uint64_t>(f[1]),
                    .expression_id = number<std::uint64_t>(f[2]),
                    .subject = hex_decode(f[3]),
                    .window_start = time_from_epoch(f[4]),
                    .window_end = time_from_epoch(f[5]),
                    .precision = hex_decode(f[6]),
                    .authority = hex_decode(f[8]),
                    .interpretation_source = hex_decode(f[9]),
                },
            });
            if (status && *status != IntentionStatus::open) {
                migrated_records.push_back(EventRecord{
                    .sequence_number = seq++,
                    .recorded_at = time_from_epoch(f[4]),
                    .authority = "user",
                    .epistemic_class = EpistemicClass::user_declared,
                    .payload = EventIntentionStatusChanged{
                        .intention_id = number<std::uint64_t>(f[1]),
                        .new_status = *status,
                        .reason = "migrated from legacy state",
                        .at = time_from_epoch(f[4]),
                    },
                });
            }
        } else if (f[0] == "X" && f.size() == 9) {
            migrated_records.push_back(EventRecord{
                .sequence_number = seq++,
                .recorded_at = time_from_epoch(f[3]),
                .authority = "core",
                .epistemic_class = EpistemicClass::observed,
                .payload = EventInteractionRecorded{
                    .id = number<std::uint64_t>(f[1]),
                    .intention_id = number<std::uint64_t>(f[2]),
                    .timestamp = time_from_epoch(f[3]),
                    .message = hex_decode(f[4]),
                    .reason = hex_decode(f[5]),
                    .decision = hex_decode(f[6]),
                    .formulation_source = hex_decode(f[7]),
                },
            });
        }
    }
    check_input.close();

    // Create .bak backup
    const auto backup_path = path_.string() + ".bak";
    std::filesystem::copy_file(path_, backup_path, std::filesystem::copy_options::overwrite_existing);
    chmod(backup_path.c_str(), S_IRUSR | S_IWUSR);

    // Atomic write to temporary file then rename
    const auto tmp_path = path_.string() + ".tmp";
    {
        std::ofstream output(tmp_path, std::ios::trunc);
        if (!output) throw std::runtime_error("could not open temporary file for migration: " + tmp_path);
        chmod(tmp_path.c_str(), S_IRUSR | S_IWUSR);
        output << "LUME-LEDGER\t1\n";
        for (const auto& record : migrated_records) {
            output << record.sequence_number << '\t'
                   << epoch(record.recorded_at) << '\t'
                   << hex_encode(record.authority) << '\t'
                   << hex_encode(to_string(record.epistemic_class)) << '\t';
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
                } else if constexpr (std::is_same_v<T, EventInteractionRecorded>) {
                    output << "INTERACTION\t" << event.id << '\t' << event.intention_id << '\t'
                           << hex_encode(event.message) << '\t' << hex_encode(event.reason) << '\t'
                           << hex_encode(event.decision) << '\t' << hex_encode(event.formulation_source);
                }
            }, record.payload);
            output << '\n';
        }
        output.flush();
    }
    std::filesystem::rename(tmp_path, path_);
    chmod(path_.c_str(), S_IRUSR | S_IWUSR);
    return true;
}

void Ledger::append(const EventRecord& record) {
    append_batch({record});
}

void Ledger::append_batch(std::vector<EventRecord> records) {
    if (records.empty()) return;

    with_exclusive_lock([&] {
        const auto existing = read_all();
        std::uint64_t max_seq = 0;
        for (const auto& rec : existing) {
            max_seq = std::max(max_seq, rec.sequence_number);
        }

        bool is_new_file = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

        if (path_.has_parent_path()) secure_directory(path_.parent_path());

        std::ofstream output(path_, std::ios::app);
        if (!output) throw std::runtime_error("could not open ledger for writing: " + path_.string());
        chmod(path_.c_str(), S_IRUSR | S_IWUSR);

        if (is_new_file) {
            output << "LUME-LEDGER\t1\n";
        }

        for (auto& record : records) {
            // Sequence numbers are managed EXCLUSIVELY by Ledger
            record.sequence_number = ++max_seq;

            output << record.sequence_number << '\t'
                   << epoch(record.recorded_at) << '\t'
                   << hex_encode(record.authority) << '\t'
                   << hex_encode(to_string(record.epistemic_class)) << '\t';

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
                } else if constexpr (std::is_same_v<T, EventAutomationCreated>) {
                    output << "AUTO_CREATE\t" << event.id << '\t'
                           << hex_encode(event.title) << '\t'
                           << hex_encode(event.trigger_when) << '\t'
                           << hex_encode(event.condition_if) << '\t'
                           << hex_encode(event.action_then) << '\t'
                           << hex_encode(event.authority) << '\t'
                           << event.status;
                } else if constexpr (std::is_same_v<T, EventAutomationStatusChanged>) {
                    output << "AUTO_STATUS\t" << event.automation_id << '\t'
                           << event.new_status << '\t'
                           << hex_encode(event.reason);
                } else if constexpr (std::is_same_v<T, EventAutomationTriggered>) {
                    output << "AUTO_TRIG\t" << event.automation_id << '\t' << hex_encode(event.explanation);
                } else if constexpr (std::is_same_v<T, EventNotificationEmitted>) {
                    output << "NOTIF_EMIT\t" << event.id << '\t'
                           << event.automation_id << '\t'
                           << hex_encode(event.title) << '\t'
                           << hex_encode(event.message) << '\t'
                           << hex_encode(event.action_type) << '\t'
                           << (event.reference_id ? std::to_string(*event.reference_id) : "-");
                }
            }, record.payload);

            output << '\n';
        }
        output.flush();
        if (!output) throw std::runtime_error("failed to write records to ledger");
        return true;
    });
}

std::vector<EventRecord> Ledger::read_all() const {
    std::vector<EventRecord> records;
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0) return records;

    std::ifstream input(path_);
    if (!input) throw std::runtime_error("could not read ledger file: " + path_.string());

    std::string header;
    if (!std::getline(input, header)) return records;

    if (header == "LUME\t1") {
        input.close();
        migrate_if_needed();
        return read_all();
    }

    if (header != "LUME-LEDGER\t1") {
        throw std::runtime_error("unsupported ledger header: " + header);
    }

    std::string line;
    std::size_t line_no = 1;
    std::uint64_t last_seq = 0;

    while (std::getline(input, line)) {
        ++line_no;
        if (line.empty()) continue;
        const auto f = split_tabs(line);
        if (f.size() < 4) throw std::runtime_error("malformed ledger record at line " + std::to_string(line_no));

        const auto seq = number<std::uint64_t>(f[0]);
        if (seq <= last_seq) {
            throw std::runtime_error("non-monotonic sequence number at line " + std::to_string(line_no) + ": " + std::to_string(seq) + " <= " + std::to_string(last_seq));
        }
        last_seq = seq;

        // Detect 4-field or 5-field prefix (with epistemic class)
        std::size_t type_idx = 3;
        std::string authority = hex_decode(f[2]);
        EpistemicClass epistemic = EpistemicClass::user_declared;

        if (f.size() >= 5 && (f[4] == "EXPR" || f[4] == "INT_DERIVED" || f[4] == "INT_STATUS" ||
                              f[4] == "INT_DEFERRED" || f[4] == "INTERACTION" || f[4] == "PLAN_PROP" ||
                              f[4] == "PLAN_APPL" || f[4] == "PLAN_DISC" || f[4] == "AUTO_CREATE" ||
                              f[4] == "AUTO_STATUS" || f[4] == "AUTO_TRIG" || f[4] == "NOTIF_EMIT")) {
            type_idx = 4;
            const auto ep_opt = epistemic_class_from_string(hex_decode(f[3]));
            if (ep_opt) epistemic = *ep_opt;
        }

        const std::string& type = f[type_idx];
        const std::size_t p = type_idx + 1; // first payload argument index

        EventRecord record{
            .sequence_number = seq,
            .recorded_at = time_from_epoch(f[1]),
            .authority = authority,
            .epistemic_class = epistemic,
            .payload = EventExpressionRecorded{},
        };

        if (type == "EXPR") {
            if (f.size() < p + 2) throw std::runtime_error("malformed EXPR record at line " + std::to_string(line_no));
            record.payload = EventExpressionRecorded{
                .id = number<std::uint64_t>(f[p]),
                .timestamp = record.recorded_at,
                .text = hex_decode(f[p + 1]),
            };
        } else if (type == "INT_DERIVED") {
            if (f.size() < p + 8) throw std::runtime_error("malformed INT_DERIVED record at line " + std::to_string(line_no));
            record.payload = EventIntentionDerived{
                .id = number<std::uint64_t>(f[p]),
                .expression_id = number<std::uint64_t>(f[p + 1]),
                .subject = hex_decode(f[p + 2]),
                .window_start = time_from_epoch(f[p + 3]),
                .window_end = time_from_epoch(f[p + 4]),
                .precision = hex_decode(f[p + 5]),
                .authority = hex_decode(f[p + 6]),
                .interpretation_source = hex_decode(f[p + 7]),
            };
        } else if (type == "INT_STATUS") {
            if (f.size() < p + 3) throw std::runtime_error("malformed INT_STATUS record at line " + std::to_string(line_no));
            const auto st = intention_status_from_string(f[p + 1]);
            if (!st) throw std::runtime_error("invalid intention status in ledger at line " + std::to_string(line_no) + ": " + f[p + 1]);
            record.payload = EventIntentionStatusChanged{
                .intention_id = number<std::uint64_t>(f[p]),
                .new_status = *st,
                .reason = hex_decode(f[p + 2]),
                .at = record.recorded_at,
            };
        } else if (type == "INT_DEFERRED") {
            if (f.size() < p + 5) throw std::runtime_error("malformed INT_DEFERRED record at line " + std::to_string(line_no));
            record.payload = EventIntentionDeferred{
                .intention_id = number<std::uint64_t>(f[p]),
                .new_start = time_from_epoch(f[p + 1]),
                .new_end = time_from_epoch(f[p + 2]),
                .precision = hex_decode(f[p + 3]),
                .reason = hex_decode(f[p + 4]),
                .at = record.recorded_at,
            };
        } else if (type == "INTERACTION") {
            if (f.size() < p + 6) throw std::runtime_error("malformed INTERACTION record at line " + std::to_string(line_no));
            record.payload = EventInteractionRecorded{
                .id = number<std::uint64_t>(f[p]),
                .intention_id = number<std::uint64_t>(f[p + 1]),
                .timestamp = record.recorded_at,
                .message = hex_decode(f[p + 2]),
                .reason = hex_decode(f[p + 3]),
                .decision = hex_decode(f[p + 4]),
                .formulation_source = hex_decode(f[p + 5]),
            };
        } else if (type == "PLAN_PROP") {
            if (f.size() < p + 6) throw std::runtime_error("malformed PLAN_PROP record at line " + std::to_string(line_no));
            record.payload = EventPlanProposed{
                .id = number<std::uint64_t>(f[p]),
                .created_at = record.recorded_at,
                .horizon = hex_decode(f[p + 1]),
                .summary = hex_decode(f[p + 2]),
                .blocks = decode_blocks(hex_decode(f[p + 3])),
                .points_of_attention = decode_strings(hex_decode(f[p + 4])),
                .status = f[p + 5],
                .source = f.size() >= p + 7 ? hex_decode(f[p + 6]) : "",
            };
        } else if (type == "PLAN_APPL") {
            if (f.size() < p + 2) throw std::runtime_error("malformed PLAN_APPL record at line " + std::to_string(line_no));
            record.payload = EventPlanApplied{
                .plan_id = number<std::uint64_t>(f[p]),
                .applied_at = record.recorded_at,
                .reason = hex_decode(f[p + 1]),
            };
        } else if (type == "PLAN_DISC") {
            if (f.size() < p + 2) throw std::runtime_error("malformed PLAN_DISC record at line " + std::to_string(line_no));
            record.payload = EventPlanDiscarded{
                .plan_id = number<std::uint64_t>(f[p]),
                .discarded_at = record.recorded_at,
                .reason = hex_decode(f[p + 1]),
            };
        } else if (type == "AUTO_CREATE") {
            if (f.size() < p + 7) throw std::runtime_error("malformed AUTO_CREATE record at line " + std::to_string(line_no));
            record.payload = EventAutomationCreated{
                .id = number<std::uint64_t>(f[p]),
                .created_at = record.recorded_at,
                .title = hex_decode(f[p + 1]),
                .trigger_when = hex_decode(f[p + 2]),
                .condition_if = hex_decode(f[p + 3]),
                .action_then = hex_decode(f[p + 4]),
                .authority = hex_decode(f[p + 5]),
                .status = f[p + 6],
            };
        } else if (type == "AUTO_STATUS") {
            if (f.size() < p + 3) throw std::runtime_error("malformed AUTO_STATUS record at line " + std::to_string(line_no));
            record.payload = EventAutomationStatusChanged{
                .automation_id = number<std::uint64_t>(f[p]),
                .new_status = f[p + 1],
                .reason = hex_decode(f[p + 2]),
                .at = record.recorded_at,
            };
        } else if (type == "AUTO_TRIG") {
            if (f.size() < p + 2) throw std::runtime_error("malformed AUTO_TRIG record at line " + std::to_string(line_no));
            record.payload = EventAutomationTriggered{
                .automation_id = number<std::uint64_t>(f[p]),
                .triggered_at = record.recorded_at,
                .explanation = hex_decode(f[p + 1]),
            };
        } else if (type == "NOTIF_EMIT") {
            if (f.size() < p + 6) throw std::runtime_error("malformed NOTIF_EMIT record at line " + std::to_string(line_no));
            std::optional<std::uint64_t> ref_id;
            if (f[p + 5] != "-") ref_id = number<std::uint64_t>(f[p + 5]);
            record.payload = EventNotificationEmitted{
                .id = number<std::uint64_t>(f[p]),
                .automation_id = number<std::uint64_t>(f[p + 1]),
                .emitted_at = record.recorded_at,
                .title = hex_decode(f[p + 2]),
                .message = hex_decode(f[p + 3]),
                .action_type = hex_decode(f[p + 4]),
                .reference_id = ref_id,
            };
        } else {
            throw std::runtime_error("unknown event type in ledger record at line " + std::to_string(line_no) + ": " + type);
        }
        records.push_back(std::move(record));
    }
    return records;
}

State Ledger::project_state() const {
    State state;
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
                    .epistemic_class = record.epistemic_class,
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
                    .epistemic_class = record.epistemic_class,
                    .allocated_plan_start = std::nullopt,
                    .allocated_plan_end = std::nullopt,
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
                    if (event.precision == "plan_slot") {
                        it->allocated_plan_start = event.new_start;
                        it->allocated_plan_end = event.new_end;
                    } else {
                        it->window_start = event.new_start;
                        it->window_end = event.new_end;
                        it->precision = event.precision;
                    }
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
                    .epistemic_class = record.epistemic_class,
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
                    .as_of_sequence = record.sequence_number,
                    .basis_intentions_digest = "",
                    .epistemic_class = record.epistemic_class,
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
            } else if constexpr (std::is_same_v<T, EventAutomationCreated>) {
                max_id = std::max(max_id, event.id);
                state.automation_proposals.push_back(AutomationProposal{
                    .id = event.id,
                    .created_at = event.created_at,
                    .title = event.title,
                    .trigger_when = event.trigger_when,
                    .condition_if = event.condition_if,
                    .action_then = event.action_then,
                    .authority = event.authority,
                    .status = event.status,
                    .last_triggered_at = std::nullopt,
                    .epistemic_class = record.epistemic_class,
                });
            } else if constexpr (std::is_same_v<T, EventAutomationStatusChanged>) {
                auto it = std::find_if(state.automation_proposals.begin(), state.automation_proposals.end(),
                                       [&](const auto& a) { return a.id == event.automation_id; });
                if (it != state.automation_proposals.end()) {
                    it->status = event.new_status;
                }
            } else if constexpr (std::is_same_v<T, EventAutomationTriggered>) {
                auto it = std::find_if(state.automation_proposals.begin(), state.automation_proposals.end(),
                                       [&](const auto& a) { return a.id == event.automation_id; });
                if (it != state.automation_proposals.end()) {
                    it->last_triggered_at = event.triggered_at;
                }
            } else if constexpr (std::is_same_v<T, EventNotificationEmitted>) {
                max_id = std::max(max_id, event.id);
                state.notifications.push_back(NotificationRecord{
                    .id = event.id,
                    .automation_id = event.automation_id,
                    .emitted_at = event.emitted_at,
                    .title = event.title,
                    .message = event.message,
                    .action_type = event.action_type,
                    .reference_id = event.reference_id,
                    .epistemic_class = record.epistemic_class,
                });
            }
        }, record.payload);
    }

    state.next_id = max_id + 1;
    return state;
}

}  // namespace lume
