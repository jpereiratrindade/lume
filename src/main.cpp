#include "lume/assistant.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::filesystem::path default_state_path() {
    if (const char* explicit_path = std::getenv("LUME_STATE_FILE")) return explicit_path;
    if (const char* state_home = std::getenv("XDG_STATE_HOME")) {
        return std::filesystem::path(state_home) / "lume" / "state.lume";
    }
    if (const char* user_home = std::getenv("HOME")) {
        return std::filesystem::path(user_home) / ".local" / "state" / "lume" / "state.lume";
    }
    return std::filesystem::path(".lume") / "state.lume";
}

std::string join(const std::vector<std::string>& values, std::size_t begin) {
    std::string result;
    for (std::size_t index = begin; index < values.size(); ++index) {
        if (!result.empty()) result += ' ';
        result += values[index];
    }
    return result;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20) result += "?";
                else result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

void print_outcome(const lume::Outcome& outcome, bool explain, bool json) {
    if (json) {
        std::cout << "{\"decision\":\"" << json_escape(outcome.decision)
                  << "\",\"message\":\"" << json_escape(outcome.message)
                  << "\",\"reason\":\"" << json_escape(outcome.reason)
                  << "\",\"type\":\"" << json_escape(outcome.type)
                  << "\",\"changed\":" << (outcome.changed ? "true" : "false");

        if (outcome.plan_proposal) {
            const auto& p = *outcome.plan_proposal;
            std::cout << ",\"plan_proposal\":{\"id\":" << p.id
                      << ",\"horizon\":\"" << json_escape(p.horizon) << "\""
                      << ",\"summary\":\"" << json_escape(p.summary) << "\""
                      << ",\"status\":\"" << json_escape(p.status) << "\""
                      << ",\"source\":\"" << json_escape(p.source) << "\""
                      << ",\"blocks\":[";
            for (std::size_t i = 0; i < p.blocks.size(); ++i) {
                const auto& b = p.blocks[i];
                if (i > 0) std::cout << ",";
                std::cout << "{\"title\":\"" << json_escape(b.title) << "\""
                          << ",\"start\":\"" << json_escape(lume::format_time(b.start)) << "\""
                          << ",\"end\":\"" << json_escape(lume::format_time(b.end)) << "\""
                          << ",\"category\":\"" << json_escape(b.category) << "\""
                          << ",\"intention_id\":" << b.intention_id << "}";
            }
            std::cout << "],\"points_of_attention\":[";
            for (std::size_t i = 0; i < p.points_of_attention.size(); ++i) {
                if (i > 0) std::cout << ",";
                std::cout << "\"" << json_escape(p.points_of_attention[i]) << "\"";
            }
            std::cout << "]}";
        }

        if (outcome.automation_proposal) {
            const auto& a = *outcome.automation_proposal;
            std::cout << ",\"automation_proposal\":{\"id\":" << a.id
                      << ",\"title\":\"" << json_escape(a.title) << "\""
                      << ",\"trigger_when\":\"" << json_escape(a.trigger_when) << "\""
                      << ",\"condition_if\":\"" << json_escape(a.condition_if) << "\""
                      << ",\"action_then\":\"" << json_escape(a.action_then) << "\""
                      << ",\"authority\":\"" << json_escape(a.authority) << "\""
                      << ",\"status\":\"" << json_escape(a.status) << "\"}";
        }

        if (!outcome.emitted_notifications.empty()) {
            std::cout << ",\"emitted_notifications\":[";
            for (std::size_t i = 0; i < outcome.emitted_notifications.size(); ++i) {
                const auto& n = outcome.emitted_notifications[i];
                if (i > 0) std::cout << ",";
                std::cout << "{\"id\":" << n.id
                          << ",\"automation_id\":" << n.automation_id
                          << ",\"emitted_at\":\"" << json_escape(lume::format_time(n.emitted_at)) << "\""
                          << ",\"title\":\"" << json_escape(n.title) << "\""
                          << ",\"message\":\"" << json_escape(n.message) << "\""
                          << ",\"action_type\":\"" << json_escape(n.action_type) << "\"";
                if (n.reference_id) std::cout << ",\"reference_id\":" << *n.reference_id;
                std::cout << "}";
            }
            std::cout << "]";
        }

        if (outcome.attention_candidate) {
            const auto& att = *outcome.attention_candidate;
            std::cout << ",\"attention_candidate\":{\"intention_id\":" << att.intention_id
                      << ",\"subject\":\"" << json_escape(att.subject) << "\""
                      << ",\"window_start\":\"" << json_escape(lume::format_time(att.window_start)) << "\""
                      << ",\"window_end\":\"" << json_escape(lume::format_time(att.window_end)) << "\""
                      << ",\"relevance_reason\":\"" << json_escape(att.relevance_reason) << "\""
                      << ",\"is_active_now\":" << (att.is_active_now ? "true" : "false") << "}";
        }

        std::cout << "}\n";
        return;
    }

    if (outcome.plan_proposal) {
        const auto& p = *outcome.plan_proposal;
        std::cout << "Proposta de Planejamento [" << p.horizon << "] #" << p.id << ":\n";
        std::cout << p.summary << "\n\n";
        for (const auto& b : p.blocks) {
            std::cout << "  • " << lume::format_time(b.start) << " - " << lume::format_time(b.end)
                      << " | " << b.title << " [" << b.category << "]\n";
        }
        if (!p.points_of_attention.empty()) {
            std::cout << "\nPontos de atenção:\n";
            for (const auto& pt : p.points_of_attention) {
                std::cout << "  - " << pt << "\n";
            }
        }
        std::cout << "\nPara aplicar: lume apply-plan " << p.id << "\n";
        return;
    }

    if (!outcome.emitted_notifications.empty()) {
        for (const auto& n : outcome.emitted_notifications) {
            std::cout << "[Notificação] " << n.title << ": " << n.message << "\n";
        }
    }

    if (!outcome.message.empty()) std::cout << outcome.message << '\n';
    else std::cout << outcome.decision << '\n';
    if (explain) std::cout << "Por quê: " << outcome.reason << '\n';
}

void print_help() {
    std::cout
        << "Lume — um assistente contextual local\n\n"
        << "Uso:\n"
        << "  lume say <expressão>                    preserva uma expressão\n"
        << "  lume observe                            observa o momento e pode ficar em silêncio\n"
        << "  lume reply <resposta>                   responde à última interação\n"
        << "  lume plan <horizonte/pedido>            gera uma proposta de planejamento estruturado\n"
        << "  lume apply-plan <id>                    aplica uma proposta aprovada ao ledger\n"
        << "  lume discard-plan <id>                  descarta uma proposta de planejamento\n"
        << "  lume tick                               executa um ciclo de avaliação de automações\n"
        << "  lume daemon [--interval-sec N]          executa o daemon de monitoramento contínuo\n"
        << "  lume automations                        lista automações ativas e histórico\n"
        << "  lume create-automation <t> <w> <c> <a>  cria nova rotina de automação\n"
        << "  lume toggle-automation <id> <status>    ativa ou pausa uma automação\n"
        << "  lume trigger-automation <id>            dispara uma automação imediatamente\n"
        << "  lume create-intention <assunto> [i] [f] cria uma intenção\n"
        << "  lume update-intention <id> <a> <i> <f>  edita uma intenção\n"
        << "  lume delete-intention <id>               exclui uma intenção da projeção\n"
        << "  lume why                                explica a última interação\n"
        << "  lume doctor                             mostra o provedor linguístico ativo\n"
        << "  lume inspect                            mostra fatos, intenções e proveniência\n"
        << "  lume                                    inicia uma conversa\n\n"
        << "Opções: --at AAAA-MM-DDTHH:MM[:SS], --state CAMINHO, --explain, --json\n";
}

lume::TimePoint now() {
    return lume::TimePoint{
        std::chrono::duration_cast<std::chrono::seconds>(lume::Clock::now().time_since_epoch())};
}

void conversation(lume::Assistant& assistant) {
    auto outcome = assistant.observe(now());
    bool awaiting_reply = outcome.decision == "INTERACT" || outcome.decision == "CLARIFY";
    if (awaiting_reply) std::cout << "Lume: " << outcome.message << '\n';
    else std::cout << "Lume: Estou aqui. O que está acontecendo agora?\n";
    std::cout << "      (/sair, /observar, /porquê, /inspecionar, /planejar)\n";

    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line == "/sair") break;
        if (line == "/inspecionar") {
            std::cout << assistant.inspect();
            continue;
        }
        if (line == "/porquê" || line == "/porque") {
            std::cout << "Lume: " << assistant.explain_last() << '\n';
            continue;
        }
        if (line == "/planejar") outcome = assistant.plan("morning", now());
        else if (line == "/observar") outcome = assistant.observe(now());
        else if (awaiting_reply) outcome = assistant.reply(line, now());
        else outcome = assistant.say(line, now());
        awaiting_reply = outcome.decision == "INTERACT" || outcome.decision == "CLARIFY";
        print_outcome(outcome, false, false);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> positional;
        auto state_path = default_state_path();
        std::optional<lume::TimePoint> specified_time;
        bool explain = false;
        bool json = false;
        int interval_sec = 60;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--state") {
                if (++index >= argc) throw std::runtime_error("--state requer um caminho");
                state_path = argv[index];
            } else if (argument == "--at") {
                if (++index >= argc) throw std::runtime_error("--at requer data e hora");
                specified_time = lume::parse_time(argv[index]);
                if (!specified_time) throw std::runtime_error("data inválida em --at");
            } else if (argument == "--interval-sec") {
                if (++index >= argc) throw std::runtime_error("--interval-sec requer segundos");
                interval_sec = std::stoi(argv[index]);
            } else if (argument == "--explain") {
                explain = true;
            } else if (argument == "--json") {
                json = true;
            } else if (argument == "--help" || argument == "-h") {
                print_help();
                return 0;
            } else {
                positional.push_back(argument);
            }
        }

        lume::Assistant assistant{lume::Ledger{state_path}, lume::make_configured_language()};
        if (positional.empty()) {
            conversation(assistant);
            return 0;
        }
        const auto moment = specified_time.value_or(now());
        const auto& command = positional.front();
        if (command == "inspect") std::cout << assistant.inspect(moment);
        else if (command == "why") std::cout << assistant.explain_last() << '\n';
        else if (command == "doctor") {
            std::cout << "Provedor linguístico: " << assistant.language_name() << '\n'
                      << "Ledger: " << state_path << '\n';
        }
        else if (command == "observe") print_outcome(assistant.observe(moment), explain, json);
        else if (command == "say") print_outcome(assistant.say(join(positional, 1), moment), explain, json);
        else if (command == "reply") print_outcome(assistant.reply(join(positional, 1), moment), explain, json);
        else if (command == "plan") {
            const auto horizon = positional.size() > 1 ? join(positional, 1) : "morning";
            print_outcome(assistant.plan(horizon, moment), explain, json);
        }
        else if (command == "apply-plan" && positional.size() > 1) {
            const auto plan_id = std::stoull(positional[1]);
            print_outcome(assistant.apply_plan(plan_id, moment), explain, json);
        }
        else if (command == "discard-plan" && positional.size() > 1) {
            const auto plan_id = std::stoull(positional[1]);
            print_outcome(assistant.discard_plan(plan_id, moment), explain, json);
        }
        else if (command == "tick") {
            print_outcome(assistant.tick(moment), explain, json);
        }
        else if (command == "automations") {
            const auto state = assistant.ledger().project_state();
            if (json) {
                std::cout << "{\"automations\":[";
                for (std::size_t i = 0; i < state.automation_proposals.size(); ++i) {
                    const auto& a = state.automation_proposals[i];
                    if (i > 0) std::cout << ",";
                    std::cout << "{\"id\":" << a.id
                              << ",\"title\":\"" << json_escape(a.title) << "\""
                              << ",\"trigger_when\":\"" << json_escape(a.trigger_when) << "\""
                              << ",\"condition_if\":\"" << json_escape(a.condition_if) << "\""
                              << ",\"action_then\":\"" << json_escape(a.action_then) << "\""
                              << ",\"authority\":\"" << json_escape(a.authority) << "\""
                              << ",\"status\":\"" << json_escape(a.status) << "\""
                              << ",\"last_triggered_at\":" << (a.last_triggered_at ? ("\"" + json_escape(lume::format_time(*a.last_triggered_at)) + "\"") : "null")
                              << "}";
                }
                std::cout << "],\"notifications\":[";
                for (std::size_t i = 0; i < state.notifications.size(); ++i) {
                    const auto& n = state.notifications[i];
                    if (i > 0) std::cout << ",";
                    std::cout << "{\"id\":" << n.id
                              << ",\"automation_id\":" << n.automation_id
                              << ",\"emitted_at\":\"" << json_escape(lume::format_time(n.emitted_at)) << "\""
                              << ",\"title\":\"" << json_escape(n.title) << "\""
                              << ",\"message\":\"" << json_escape(n.message) << "\""
                              << ",\"action_type\":\"" << json_escape(n.action_type) << "\"";
                    if (n.reference_id) std::cout << ",\"reference_id\":" << *n.reference_id;
                    std::cout << "}";
                }
                std::cout << "]}\n";
            } else {
                std::cout << "Automações Registradas (" << state.automation_proposals.size() << "):\n";
                for (const auto& a : state.automation_proposals) {
                    std::cout << "  #" << a.id << " [" << a.status << "] " << a.title
                              << " | Quando: " << a.trigger_when
                              << " | Ação: " << a.action_then << "\n";
                }
                if (!state.notifications.empty()) {
                    std::cout << "\nNotificações Recentes (" << state.notifications.size() << "):\n";
                    for (const auto& n : state.notifications) {
                        std::cout << "  [" << lume::format_time(n.emitted_at) << "] " << n.title << ": " << n.message << "\n";
                    }
                }
            }
        }
        else if (command == "create-automation" && positional.size() >= 5) {
            const auto title = positional[1];
            const auto trigger_when = positional[2];
            const auto condition_if = positional[3];
            const auto action_then = positional[4];
            const auto authority = positional.size() > 5 ? positional[5] : "suggest_only";
            print_outcome(assistant.create_automation(title, trigger_when, condition_if, action_then, authority, moment), explain, json);
        }
        else if (command == "toggle-automation" && positional.size() >= 3) {
            const auto id = std::stoull(positional[1]);
            const auto status = positional[2];
            print_outcome(assistant.toggle_automation(id, status, moment), explain, json);
        }
        else if (command == "trigger-automation" && positional.size() >= 2) {
            const auto id = std::stoull(positional[1]);
            print_outcome(assistant.trigger_automation(id, moment), explain, json);
        }
        else if (command == "create-intention" && positional.size() >= 2) {
            const auto subject = positional[1];
            const auto start = positional.size() >= 3 ? lume::parse_time(positional[2]).value_or(moment) : moment;
            const auto end = positional.size() >= 4 ? lume::parse_time(positional[3]).value_or(start + std::chrono::hours(2)) : start + std::chrono::hours(2);
            print_outcome(assistant.create_intention(subject, start, end, moment), explain, json);
        }
        else if (command == "update-intention" && positional.size() >= 5) {
            const auto id = std::stoull(positional[1]);
            const auto subject = positional[2];
            const auto start = lume::parse_time(positional[3]);
            const auto end = lume::parse_time(positional[4]);
            if (!start || !end) throw std::runtime_error("Janela temporal inválida para edição.");
            print_outcome(assistant.update_intention(id, subject, *start, *end, moment), explain, json);
        }
        else if (command == "delete-intention" && positional.size() >= 2) {
            const auto id = std::stoull(positional[1]);
            print_outcome(assistant.delete_intention(id, moment), explain, json);
        }
        else if (command == "complete-intention" && positional.size() >= 2) {
            const auto id = std::stoull(positional[1]);
            print_outcome(assistant.update_intention_status(id, lume::IntentionStatus::completed, "concluída via comando terminal", moment), explain, json);
        }
        else if (command == "dismiss-intention" && positional.size() >= 2) {
            const auto id = std::stoull(positional[1]);
            print_outcome(assistant.update_intention_status(id, lume::IntentionStatus::dismissed, "descartada via comando terminal", moment), explain, json);
        }
        else if (command == "defer-intention" && positional.size() >= 2) {
            const auto id = std::stoull(positional[1]);
            const auto new_start = positional.size() >= 3 ? lume::parse_time(positional[2]).value_or(moment + std::chrono::hours(1)) : moment + std::chrono::hours(1);
            const auto new_end = positional.size() >= 4 ? lume::parse_time(positional[3]).value_or(new_start + std::chrono::hours(2)) : new_start + std::chrono::hours(2);
            print_outcome(assistant.defer_intention(id, new_start, new_end, "adiada via comando terminal", moment), explain, json);
        }
        else if (command == "daemon") {
            std::cout << "Lume daemon iniciado. Avaliando a cada " << interval_sec << "s...\n";
            while (true) {
                const auto current = now();
                const auto tick_outcome = assistant.tick(current);
                if (tick_outcome.changed) {
                    print_outcome(tick_outcome, explain, json);
                }
                std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
            }
        }
        else print_outcome(assistant.say(join(positional, 0), moment), explain, json);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lume: " << error.what() << '\n';
        return 1;
    }
}
