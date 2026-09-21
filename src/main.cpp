#include "lume/assistant.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

    if (!outcome.message.empty()) std::cout << outcome.message << '\n';
    else std::cout << outcome.decision << '\n';
    if (explain) std::cout << "Por quê: " << outcome.reason << '\n';
}

void print_help() {
    std::cout
        << "Lume — um assistente contextual local\n\n"
        << "Uso:\n"
        << "  lume say <expressão>          preserva uma expressão\n"
        << "  lume observe                  observa o momento e pode ficar em silêncio\n"
        << "  lume reply <resposta>         responde à última interação\n"
        << "  lume plan <horizonte/pedido>  gera uma proposta de planejamento estruturado\n"
        << "  lume apply-plan <id>          aplica uma proposta aprovada ao ledger\n"
        << "  lume discard-plan <id>        descarta uma proposta de planejamento\n"
        << "  lume why                      explica a última interação\n"
        << "  lume doctor                   mostra o provedor linguístico ativo\n"
        << "  lume inspect                  mostra fatos, intenções e proveniência\n"
        << "  lume                          inicia uma conversa\n\n"
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

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--state") {
                if (++index >= argc) throw std::runtime_error("--state requer um caminho");
                state_path = argv[index];
            } else if (argument == "--at") {
                if (++index >= argc) throw std::runtime_error("--at requer data e hora");
                specified_time = lume::parse_time(argv[index]);
                if (!specified_time) throw std::runtime_error("data inválida em --at");
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
        if (command == "inspect") std::cout << assistant.inspect();
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
        else print_outcome(assistant.say(join(positional, 0), moment), explain, json);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lume: " << error.what() << '\n';
        return 1;
    }
}
