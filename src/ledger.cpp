#include "lume/ledger.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lume {
namespace {

using Json = nlohmann::json;

// --- POSIX Permissions Enforcement ---
void ensure_secure_permissions(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        chmod(path.parent_path().c_str(), S_IRWXU); // 0700
    }
    if (std::filesystem::exists(path)) {
        chmod(path.c_str(), S_IRUSR | S_IWUSR); // 0600
    }
}

// --- Thread-Safe Reentrant Process/File Lock ---
struct LockInfo {
    int fd{-1};
    int count{0};
    std::thread::id owner{};
    int waiters{0};
};

std::mutex g_lock_mutex;
std::condition_variable g_lock_cv;
std::unordered_map<std::string, LockInfo> g_locks;

int acquire_reentrant_lock(const std::string& path) {
    if (path.empty()) return -1;
    const auto current_thread = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(g_lock_mutex);

    while (true) {
        auto& info = g_locks[path];
        if (info.count == 0) {
            info.owner = current_thread;
            info.count = 1;

            if (info.fd < 0) {
                ensure_secure_permissions(path);
                info.fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
                if (info.fd < 0) {
                    g_locks.erase(path);
                    throw std::runtime_error("Não foi possível criar o arquivo de lock: " + path);
                }
                chmod(path.c_str(), S_IRUSR | S_IWUSR);
            }

            lk.unlock();
            struct flock fl{};
            fl.l_type = F_WRLCK;
            fl.l_whence = SEEK_SET;
            fl.l_start = 0;
            fl.l_len = 0;
            while (fcntl(info.fd, F_SETLKW, &fl) < 0) {
                if (errno == EINTR) continue;
                lk.lock();
                info.count = 0;
                info.owner = {};
                g_lock_cv.notify_all();
                throw std::runtime_error("Falha ao adquirir trava exclusiva do processo: " + path);
            }
            return info.fd;
        }

        if (info.owner == current_thread) {
            info.count++;
            return info.fd;
        }

        info.waiters++;
        g_lock_cv.wait(lk, [&]() {
            auto it = g_locks.find(path);
            return it == g_locks.end() || it->second.count == 0;
        });
        auto& updated = g_locks[path];
        updated.waiters--;
    }
}

void release_reentrant_lock(const std::string& path) {
    if (path.empty()) return;
    const auto current_thread = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(g_lock_mutex);

    auto it = g_locks.find(path);
    if (it == g_locks.end() || it->second.owner != current_thread || it->second.count <= 0) {
        return;
    }

    it->second.count--;
    if (it->second.count == 0) {
        struct flock fl{};
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 0;
        fcntl(it->second.fd, F_SETLK, &fl);
        it->second.owner = {};

        if (it->second.waiters == 0) {
            close(it->second.fd);
            g_locks.erase(it);
        }
        lk.unlock();
        g_lock_cv.notify_one();
    }
}

// --- SQLite Database RAII Wrapper ---
class SqliteDb {
public:
    explicit SqliteDb(const std::filesystem::path& path) {
        ensure_secure_permissions(path);
        int rc = sqlite3_open_v2(path.c_str(), &db_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
        if (rc != SQLITE_OK) {
            std::string err = db_ ? sqlite3_errmsg(db_) : "unknown error";
            if (db_) sqlite3_close(db_);
            throw std::runtime_error("Falha ao abrir banco SQLite: " + err);
        }
        ensure_secure_permissions(path);
        init_schema();
    }

    ~SqliteDb() {
        if (db_) sqlite3_close(db_);
    }

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    sqlite3* get() const noexcept { return db_; }

    void exec(const std::string& sql) {
        char* err_msg = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string err = err_msg ? err_msg : "unknown error";
            sqlite3_free(err_msg);
            throw std::runtime_error("Erro de execução SQLite (" + sql + "): " + err);
        }
    }

private:
    void init_schema() {
        // WAL mode & performance pragmas
        exec("PRAGMA journal_mode = WAL;");
        exec("PRAGMA synchronous = NORMAL;");
        exec("PRAGMA foreign_keys = ON;");
        exec("PRAGMA busy_timeout = 5000;");

        // Schema initialization
        exec(R"(
            CREATE TABLE IF NOT EXISTS schema_meta (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS events (
                sequence_number INTEGER PRIMARY KEY AUTOINCREMENT,
                recorded_at TEXT NOT NULL,
                event_type TEXT NOT NULL,
                authority TEXT NOT NULL,
                epistemic_class TEXT NOT NULL,
                payload_json TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS idx_events_type ON events(event_type);
            CREATE INDEX IF NOT EXISTS idx_events_recorded_at ON events(recorded_at);
        )");
    }

    sqlite3* db_{nullptr};
};

// --- Payload JSON Serializers & Deserializers ---

std::pair<std::string, std::string> serialize_payload(const EventPayload& payload) {
    return std::visit([](const auto& event) -> std::pair<std::string, std::string> {
        using T = std::decay_t<decltype(event)>;
        if constexpr (std::is_same_v<T, EventExpressionRecorded>) {
            Json j{
                {"id", event.id},
                {"timestamp", format_time(event.timestamp)},
                {"text", event.text},
            };
            return {"EXPR_RECORDED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventContextDerived>) {
            Json j{
                {"id", event.id},
                {"expression_id", event.expression_id},
                {"kind", event.kind},
                {"subject", event.subject},
                {"precision", event.precision},
                {"interpretation_source", event.interpretation_source},
                {"confidence", event.confidence},
            };
            return {"CONTEXT_DERIVED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventIntentionDerived>) {
            Json j{
                {"id", event.id},
                {"expression_id", event.expression_id},
                {"subject", event.subject},
                {"window_start", format_time(event.window_start)},
                {"window_end", format_time(event.window_end)},
                {"precision", event.precision},
                {"authority", event.authority},
                {"interpretation_source", event.interpretation_source},
            };
            return {"INT_DERIVED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventIntentionStatusChanged>) {
            Json j{
                {"intention_id", event.intention_id},
                {"new_status", to_string(event.new_status)},
                {"reason", event.reason},
                {"at", format_time(event.at)},
            };
            return {"INT_STATUS", j.dump()};
        } else if constexpr (std::is_same_v<T, EventIntentionDeferred>) {
            Json j{
                {"intention_id", event.intention_id},
                {"new_start", format_time(event.new_start)},
                {"new_end", format_time(event.new_end)},
                {"precision", event.precision},
                {"reason", event.reason},
                {"at", format_time(event.at)},
            };
            return {"INT_DEFERRED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventIntentionUpdated>) {
            Json j{
                {"intention_id", event.intention_id},
                {"subject", event.subject},
                {"window_start", format_time(event.window_start)},
                {"window_end", format_time(event.window_end)},
                {"precision", event.precision},
                {"reason", event.reason},
                {"at", format_time(event.at)},
            };
            return {"INT_UPDATED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventIntentionDeleted>) {
            Json j{
                {"intention_id", event.intention_id},
                {"reason", event.reason},
                {"at", format_time(event.at)},
            };
            return {"INT_DELETED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventInteractionRecorded>) {
            Json j{
                {"id", event.id},
                {"intention_id", event.intention_id},
                {"timestamp", format_time(event.timestamp)},
                {"message", event.message},
                {"reason", event.reason},
                {"decision", event.decision},
                {"formulation_source", event.formulation_source},
            };
            return {"INTERACTION", j.dump()};
        } else if constexpr (std::is_same_v<T, EventPlanProposed>) {
            Json blocks = Json::array();
            for (const auto& b : event.blocks) {
                blocks.push_back({
                    {"title", b.title},
                    {"start", format_time(b.start)},
                    {"end", format_time(b.end)},
                    {"category", b.category},
                    {"intention_id", b.intention_id},
                });
            }
            Json j{
                {"id", event.id},
                {"created_at", format_time(event.created_at)},
                {"horizon", event.horizon},
                {"summary", event.summary},
                {"blocks", blocks},
                {"points_of_attention", event.points_of_attention},
                {"status", event.status},
                {"source", event.source},
            };
            return {"PLAN_PROPOSED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventPlanApplied>) {
            Json j{
                {"plan_id", event.plan_id},
                {"applied_at", format_time(event.applied_at)},
                {"reason", event.reason},
            };
            return {"PLAN_APPLIED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventPlanDiscarded>) {
            Json j{
                {"plan_id", event.plan_id},
                {"discarded_at", format_time(event.discarded_at)},
                {"reason", event.reason},
            };
            return {"PLAN_DISCARDED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventAutomationCreated>) {
            Json j{
                {"id", event.id},
                {"created_at", format_time(event.created_at)},
                {"title", event.title},
                {"trigger_when", event.trigger_when},
                {"condition_if", event.condition_if},
                {"action_then", event.action_then},
                {"authority", event.authority},
                {"status", event.status},
            };
            return {"AUTO_CREATED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventAutomationStatusChanged>) {
            Json j{
                {"automation_id", event.automation_id},
                {"new_status", event.new_status},
                {"reason", event.reason},
                {"at", format_time(event.at)},
            };
            return {"AUTO_STATUS", j.dump()};
        } else if constexpr (std::is_same_v<T, EventAutomationTriggered>) {
            Json j{
                {"automation_id", event.automation_id},
                {"triggered_at", format_time(event.triggered_at)},
                {"explanation", event.explanation},
            };
            return {"AUTO_TRIGGERED", j.dump()};
        } else if constexpr (std::is_same_v<T, EventNotificationEmitted>) {
            Json j{
                {"id", event.id},
                {"automation_id", event.automation_id},
                {"emitted_at", format_time(event.emitted_at)},
                {"title", event.title},
                {"message", event.message},
                {"action_type", event.action_type},
                {"reference_id", event.reference_id ? Json(*event.reference_id) : Json(nullptr)},
            };
            return {"NOTIFICATION_EMITTED", j.dump()};
        }
        throw std::runtime_error("Tipo de payload de evento desconhecido.");
    }, payload);
}

EventPayload deserialize_payload(std::string_view type, const std::string& json_str) {
    const auto j = Json::parse(json_str);
    if (type == "EXPR_RECORDED") {
        return EventExpressionRecorded{
            .id = j.at("id").get<std::uint64_t>(),
            .timestamp = parse_time(j.at("timestamp").get<std::string>()).value_or(TimePoint{}),
            .text = j.at("text").get<std::string>(),
        };
    }
    if (type == "CONTEXT_DERIVED") {
        return EventContextDerived{
            .id = j.at("id").get<std::uint64_t>(),
            .expression_id = j.at("expression_id").get<std::uint64_t>(),
            .kind = j.at("kind").get<std::string>(),
            .subject = j.at("subject").get<std::string>(),
            .precision = j.at("precision").get<std::string>(),
            .interpretation_source = j.at("interpretation_source").get<std::string>(),
            .confidence = j.at("confidence").get<double>(),
        };
    }
    if (type == "INT_DERIVED") {
        return EventIntentionDerived{
            .id = j.at("id").get<std::uint64_t>(),
            .expression_id = j.at("expression_id").get<std::uint64_t>(),
            .subject = j.at("subject").get<std::string>(),
            .window_start = parse_time(j.at("window_start").get<std::string>()).value_or(TimePoint{}),
            .window_end = parse_time(j.at("window_end").get<std::string>()).value_or(TimePoint{}),
            .precision = j.at("precision").get<std::string>(),
            .authority = j.at("authority").get<std::string>(),
            .interpretation_source = j.at("interpretation_source").get<std::string>(),
        };
    }
    if (type == "INT_STATUS") {
        auto status_opt = intention_status_from_string(j.at("new_status").get<std::string>());
        if (!status_opt) throw std::runtime_error("Status de intenção inválido no payload.");
        return EventIntentionStatusChanged{
            .intention_id = j.at("intention_id").get<std::uint64_t>(),
            .new_status = *status_opt,
            .reason = j.at("reason").get<std::string>(),
            .at = parse_time(j.at("at").get<std::string>()).value_or(TimePoint{}),
        };
    }
    if (type == "INT_DEFERRED") {
        return EventIntentionDeferred{
            .intention_id = j.at("intention_id").get<std::uint64_t>(),
            .new_start = parse_time(j.at("new_start").get<std::string>()).value_or(TimePoint{}),
            .new_end = parse_time(j.at("new_end").get<std::string>()).value_or(TimePoint{}),
            .precision = j.at("precision").get<std::string>(),
            .reason = j.at("reason").get<std::string>(),
            .at = parse_time(j.at("at").get<std::string>()).value_or(TimePoint{}),
        };
    }
    if (type == "INT_UPDATED") {
        return EventIntentionUpdated{
            .intention_id = j.at("intention_id").get<std::uint64_t>(),
            .subject = j.at("subject").get<std::string>(),
            .window_start = parse_time(j.at("window_start").get<std::string>()).value_or(TimePoint{}),
            .window_end = parse_time(j.at("window_end").get<std::string>()).value_or(TimePoint{}),
            .precision = j.at("precision").get<std::string>(),
            .reason = j.at("reason").get<std::string>(),
            .at = parse_time(j.at("at").get<std::string>()).value_or(TimePoint{}),
        };
    }
    if (type == "INT_DELETED") {
        return EventIntentionDeleted{
            .intention_id = j.at("intention_id").get<std::uint64_t>(),
            .reason = j.at("reason").get<std::string>(),
            .at = parse_time(j.at("at").get<std::string>()).value_or(TimePoint{}),
        };
    }
    if (type == "INTERACTION") {
        return EventInteractionRecorded{
            .id = j.at("id").get<std::uint64_t>(),
            .intention_id = j.at("intention_id").get<std::uint64_t>(),
            .timestamp = parse_time(j.at("timestamp").get<std::string>()).value_or(TimePoint{}),
            .message = j.at("message").get<std::string>(),
            .reason = j.at("reason").get<std::string>(),
            .decision = j.at("decision").get<std::string>(),
            .formulation_source = j.at("formulation_source").get<std::string>(),
        };
    }
    if (type == "PLAN_PROPOSED") {
        std::vector<PlanBlock> blocks;
        for (const auto& bj : j.at("blocks")) {
            blocks.push_back({
                .title = bj.at("title").get<std::string>(),
                .start = parse_time(bj.at("start").get<std::string>()).value_or(TimePoint{}),
                .end = parse_time(bj.at("end").get<std::string>()).value_or(TimePoint{}),
                .category = bj.at("category").get<std::string>(),
                .intention_id = bj.at("intention_id").get<std::uint64_t>(),
            });
        }
        std::vector<std::string> attention;
        for (const auto& aj : j.at("points_of_attention")) {
            attention.push_back(aj.get<std::string>());
        }
        return EventPlanProposed{
            .id = j.at("id").get<std::uint64_t>(),
            .created_at = parse_time(j.at("created_at").get<std::string>()).value_or(TimePoint{}),
            .horizon = j.at("horizon").get<std::string>(),
            .summary = j.at("summary").get<std::string>(),
            .blocks = std::move(blocks),
            .points_of_attention = std::move(attention),
            .status = j.at("status").get<std::string>(),
            .source = j.at("source").get<std::string>(),
        };
    }
    if (type == "PLAN_APPLIED") {
        return EventPlanApplied{
            .plan_id = j.at("plan_id").get<std::uint64_t>(),
            .applied_at = parse_time(j.at("applied_at").get<std::string>()).value_or(TimePoint{}),
            .reason = j.at("reason").get<std::string>(),
        };
    }
    if (type == "PLAN_DISCARDED") {
        return EventPlanDiscarded{
            .plan_id = j.at("plan_id").get<std::uint64_t>(),
            .discarded_at = parse_time(j.at("discarded_at").get<std::string>()).value_or(TimePoint{}),
            .reason = j.at("reason").get<std::string>(),
        };
    }
    if (type == "AUTO_CREATED") {
        return EventAutomationCreated{
            .id = j.at("id").get<std::uint64_t>(),
            .created_at = parse_time(j.at("created_at").get<std::string>()).value_or(TimePoint{}),
            .title = j.at("title").get<std::string>(),
            .trigger_when = j.at("trigger_when").get<std::string>(),
            .condition_if = j.at("condition_if").get<std::string>(),
            .action_then = j.at("action_then").get<std::string>(),
            .authority = j.at("authority").get<std::string>(),
            .status = j.at("status").get<std::string>(),
        };
    }
    if (type == "AUTO_STATUS") {
        return EventAutomationStatusChanged{
            .automation_id = j.at("automation_id").get<std::uint64_t>(),
            .new_status = j.at("new_status").get<std::string>(),
            .reason = j.at("reason").get<std::string>(),
            .at = parse_time(j.at("at").get<std::string>()).value_or(TimePoint{}),
        };
    }
    if (type == "AUTO_TRIGGERED") {
        return EventAutomationTriggered{
            .automation_id = j.at("automation_id").get<std::uint64_t>(),
            .triggered_at = parse_time(j.at("triggered_at").get<std::string>()).value_or(TimePoint{}),
            .explanation = j.at("explanation").get<std::string>(),
        };
    }
    if (type == "NOTIFICATION_EMITTED") {
        std::optional<std::uint64_t> ref_id;
        if (!j.at("reference_id").is_null()) {
            ref_id = j.at("reference_id").get<std::uint64_t>();
        }
        return EventNotificationEmitted{
            .id = j.at("id").get<std::uint64_t>(),
            .automation_id = j.at("automation_id").get<std::uint64_t>(),
            .emitted_at = parse_time(j.at("emitted_at").get<std::string>()).value_or(TimePoint{}),
            .title = j.at("title").get<std::string>(),
            .message = j.at("message").get<std::string>(),
            .action_type = j.at("action_type").get<std::string>(),
            .reference_id = ref_id,
        };
    }
    throw std::runtime_error("Evento desconhecido no banco de dados: " + std::string(type));
}

// --- Legacy Text Ledger Migration Helper ---
bool is_sqlite_database(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path) || std::filesystem::file_size(path) < 16) return false;
    std::ifstream file(path, std::ios::binary);
    char header[16];
    file.read(header, 16);
    return std::string_view(header, 16).starts_with("SQLite format 3");
}

std::vector<std::string> split_tabs(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto pos = line.find('\t', start);
        if (pos == std::string_view::npos) {
            fields.emplace_back(line.substr(start));
            break;
        }
        fields.emplace_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

std::uint8_t hex_val(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(10 + c - 'a');
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(10 + c - 'A');
    throw std::runtime_error("Caractere hexadecimal inválido");
}

std::string legacy_hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0) throw std::runtime_error("Tamanho de hex inválido");
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<char>((hex_val(hex[i]) << 4) | hex_val(hex[i + 1])));
    }
    return out;
}

TimePoint legacy_time_from_epoch(std::string_view sec_str) {
    try {
        long long s = std::stoll(std::string(sec_str));
        return TimePoint{std::chrono::seconds{s}};
    } catch (...) {
        return TimePoint{};
    }
}

std::vector<EventRecord> parse_legacy_text_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::string line;
    if (!std::getline(file, line)) return {};

    bool is_v1_snapshot = (line == "LUME\t1");
    bool is_v1_ledger = (line == "LUME-LEDGER\t1");

    if (!is_v1_snapshot && !is_v1_ledger) {
        throw std::runtime_error("Formato de arquivo legado desconhecido: " + line);
    }

    std::vector<EventRecord> records;
    std::uint64_t next_seq = 1;

    if (is_v1_snapshot) {
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            const auto fields = split_tabs(line);
            if (fields.empty()) continue;
            const auto& tag = fields.front();
            if (tag == "E" && fields.size() >= 4) {
                const auto id = std::stoull(fields[1]);
                const auto time = legacy_time_from_epoch(fields[2]);
                const auto text = legacy_hex_decode(fields[3]);
                records.push_back({
                    .sequence_number = next_seq++,
                    .recorded_at = time,
                    .authority = "user",
                    .epistemic_class = EpistemicClass::user_declared,
                    .payload = EventExpressionRecorded{.id = id, .timestamp = time, .text = text},
                });
            } else if (tag == "I" && fields.size() >= 10) {
                const auto id = std::stoull(fields[1]);
                const auto expression_id = std::stoull(fields[2]);
                const auto subject = legacy_hex_decode(fields[3]);
                const auto start = legacy_time_from_epoch(fields[4]);
                const auto end = legacy_time_from_epoch(fields[5]);
                const auto precision = legacy_hex_decode(fields[6]);
                const auto authority = legacy_hex_decode(fields[8]);
                const auto source = legacy_hex_decode(fields[9]);
                records.push_back({
                    .sequence_number = next_seq++,
                    .recorded_at = start,
                    .authority = authority.empty() ? "user" : authority,
                    .epistemic_class = EpistemicClass::derived,
                    .payload = EventIntentionDerived{
                        .id = id,
                        .expression_id = expression_id,
                        .subject = subject,
                        .window_start = start,
                        .window_end = end,
                        .precision = precision,
                        .authority = authority.empty() ? "user" : authority,
                        .interpretation_source = source,
                    },
                });
            }
        }
        return records;
    }

    // Parse LUME-LEDGER\t1 text records
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const auto f = split_tabs(line);
        if (f.size() < 5) throw std::runtime_error("Registro truncado no ledger texto.");
        const auto seq = std::stoull(f[0]);
        const auto rec_time = parse_time(f[1]).value_or(TimePoint{});
        const auto ev_type = f[2];
        const auto authority = f[3];
        const auto epistemic_opt = epistemic_class_from_string(f[4]);
        if (!epistemic_opt) throw std::runtime_error("Classe epistêmica inválida.");
        const auto epistemic = *epistemic_opt;

        if (ev_type == "EXPR_RECORDED" && f.size() >= 8) {
            records.push_back({
                .sequence_number = seq,
                .recorded_at = rec_time,
                .authority = authority,
                .epistemic_class = epistemic,
                .payload = EventExpressionRecorded{
                    .id = std::stoull(f[5]),
                    .timestamp = parse_time(f[6]).value_or(TimePoint{}),
                    .text = f[7],
                },
            });
        } else if (ev_type == "INT_DERIVED" && f.size() >= 13) {
            records.push_back({
                .sequence_number = seq,
                .recorded_at = rec_time,
                .authority = authority,
                .epistemic_class = epistemic,
                .payload = EventIntentionDerived{
                    .id = std::stoull(f[5]),
                    .expression_id = std::stoull(f[6]),
                    .subject = f[7],
                    .window_start = parse_time(f[8]).value_or(TimePoint{}),
                    .window_end = parse_time(f[9]).value_or(TimePoint{}),
                    .precision = f[10],
                    .authority = f[11],
                    .interpretation_source = f[12],
                },
            });
        } else {
            // For other legacy types, skip or parse accordingly
        }
    }
    return records;
}

}  // namespace

FileLockGuard::FileLockGuard(std::string lock_path) : lock_path_(std::move(lock_path)) {
    if (!lock_path_.empty()) {
        acquire_reentrant_lock(lock_path_);
    }
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

void FileLockGuard::release() noexcept {
    if (!lock_path_.empty()) {
        release_reentrant_lock(lock_path_);
        lock_path_.clear();
    }
}

Ledger::Ledger(std::filesystem::path path) : path_(std::move(path)) {
    ensure_secure_permissions(path_);
    migrate_if_needed();
}

const std::filesystem::path& Ledger::path() const noexcept {
    return path_;
}

FileLockGuard Ledger::acquire_file_lock() const {
    const auto lock_path = path_.string() + ".lock";
    return FileLockGuard{lock_path};
}

bool Ledger::migrate_if_needed() const {
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0) {
        return false;
    }

    if (is_sqlite_database(path_)) {
        return false;
    }

    // It is a legacy text file! Parse records and migrate to SQLite
    auto legacy_records = parse_legacy_text_file(path_);

    const auto backup_path = path_.string() + ".bak";
    std::error_code ec;
    std::filesystem::copy_file(path_, backup_path, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(path_, ec);

    // Initialize fresh SQLite database
    SqliteDb db(path_);
    db.exec("BEGIN TRANSACTION;");

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO events (sequence_number, recorded_at, event_type, authority, epistemic_class, payload_json) VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        db.exec("ROLLBACK;");
        throw std::runtime_error("Falha ao preparar statement de migração.");
    }

    for (const auto& rec : legacy_records) {
        const auto [type_str, payload_json] = serialize_payload(rec.payload);
        const auto rec_time = format_time(rec.recorded_at);
        const auto epistemic = to_string(rec.epistemic_class);

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(rec.sequence_number));
        sqlite3_bind_text(stmt, 2, rec_time.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, type_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, rec.authority.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, epistemic.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, payload_json.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            db.exec("ROLLBACK;");
            throw std::runtime_error("Falha ao inserir evento migrado no SQLite.");
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    db.exec("COMMIT;");
    ensure_secure_permissions(path_);
    return true;
}

void Ledger::append(const EventRecord& record) {
    append_batch({record});
}

void Ledger::append_batch(std::vector<EventRecord> records) {
    if (records.empty()) return;

    FileLockGuard lock = acquire_file_lock();
    migrate_if_needed();

    SqliteDb db(path_);
    db.exec("BEGIN IMMEDIATE;");

    // Fetch maximum current sequence number
    sqlite3_stmt* max_stmt = nullptr;
    std::uint64_t next_seq = 1;
    if (sqlite3_prepare_v2(db.get(), "SELECT COALESCE(MAX(sequence_number), 0) FROM events;", -1, &max_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(max_stmt) == SQLITE_ROW) {
            next_seq = static_cast<std::uint64_t>(sqlite3_column_int64(max_stmt, 0)) + 1;
        }
        sqlite3_finalize(max_stmt);
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO events (sequence_number, recorded_at, event_type, authority, epistemic_class, payload_json) VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        db.exec("ROLLBACK;");
        throw std::runtime_error("Falha ao preparar statement de inserção SQLite.");
    }

    for (auto& rec : records) {
        rec.sequence_number = next_seq++;
        const auto [type_str, payload_json] = serialize_payload(rec.payload);
        const auto rec_time = format_time(rec.recorded_at);
        const auto epistemic = to_string(rec.epistemic_class);

        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(rec.sequence_number));
        sqlite3_bind_text(stmt, 2, rec_time.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, type_str.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, rec.authority.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, epistemic.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, payload_json.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            db.exec("ROLLBACK;");
            throw std::runtime_error("Falha ao gravar evento no SQLite.");
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    db.exec("COMMIT;");
    ensure_secure_permissions(path_);
}

std::vector<EventRecord> Ledger::read_all() const {
    if (!std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0) {
        return {};
    }

    if (!is_sqlite_database(path_)) {
        // May be unmigrated text file or corrupt file
        try {
            return parse_legacy_text_file(path_);
        } catch (const std::exception&) {
            throw std::runtime_error("Corrupção ou formato inválido de ledger: " + path_.string());
        }
    }

    SqliteDb db(path_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT sequence_number, recorded_at, event_type, authority, epistemic_class, payload_json FROM events ORDER BY sequence_number ASC;";
    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Falha ao consultar eventos no SQLite.");
    }

    std::vector<EventRecord> records;
    std::uint64_t last_seq = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto seq = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        const char* time_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* type_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* auth_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* epistemic_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* json_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));

        if (seq <= last_seq) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("Quebra de sequência monotônica no banco de eventos.");
        }
        last_seq = seq;

        auto epistemic_opt = epistemic_class_from_string(epistemic_str ? epistemic_str : "");
        if (!epistemic_opt) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("Classe epistêmica inválida encontrada no banco.");
        }

        auto payload = deserialize_payload(type_str ? type_str : "", json_str ? json_str : "{}");
        records.push_back(EventRecord{
            .sequence_number = seq,
            .recorded_at = parse_time(time_str ? time_str : "").value_or(TimePoint{}),
            .authority = auth_str ? auth_str : "core",
            .epistemic_class = *epistemic_opt,
            .payload = std::move(payload),
        });
    }

    sqlite3_finalize(stmt);
    return records;
}

State Ledger::project_state() const {
    const auto records = read_all();
    State state;
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
            } else if constexpr (std::is_same_v<T, EventContextDerived>) {
                max_id = std::max(max_id, event.id);
                state.context_items.push_back(ContextItem{
                    .id = event.id,
                    .expression_id = event.expression_id,
                    .kind = event.kind,
                    .subject = event.subject,
                    .precision = event.precision,
                    .interpretation_source = event.interpretation_source,
                    .confidence = event.confidence,
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
            } else if constexpr (std::is_same_v<T, EventIntentionUpdated>) {
                auto it = std::find_if(state.intentions.begin(), state.intentions.end(),
                                       [&](const auto& item) { return item.id == event.intention_id; });
                if (it != state.intentions.end()) {
                    it->subject = event.subject;
                    it->window_start = event.window_start;
                    it->window_end = event.window_end;
                    it->precision = event.precision;
                    it->allocated_plan_start.reset();
                    it->allocated_plan_end.reset();
                    it->last_interaction_at.reset();
                }
            } else if constexpr (std::is_same_v<T, EventIntentionDeleted>) {
                std::erase_if(state.intentions,
                              [&](const auto& item) { return item.id == event.intention_id; });
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
