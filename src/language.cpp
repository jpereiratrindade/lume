#include "lume/language.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>

namespace lume {
namespace {

void replace_all(std::string& value, std::string_view from, std::string_view to) {
    std::size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

std::string trim(std::string value) {
    const auto space = [](unsigned char character) { return std::isspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    while (!value.empty() && (value.back() == '.' || value.back() == '!' || value.back() == '?')) {
        value.pop_back();
    }
    return value;
}

std::string extract_subject(std::string_view original) {
    std::string lower(original);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const auto marker = lower.find("quero ");
    return marker == std::string::npos ? trim(std::string(original))
                                       : trim(std::string(original.substr(marker + 6)));
}

}  // namespace

std::string fold_portuguese(std::string_view input) {
    std::string value(input);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    for (const auto& [from, to] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"á", "a"}, {"à", "a"}, {"â", "a"}, {"ã", "a"}, {"é", "e"},
             {"ê", "e"}, {"í", "i"}, {"ó", "o"}, {"ô", "o"}, {"õ", "o"},
             {"ú", "u"}, {"ç", "c"}}) {
        replace_all(value, from, to);
    }
    return value;
}

InterpretationCandidate DeterministicLanguage::interpret(const InterpretationRequest& request) {
    const auto normalized = fold_portuguese(request.utterance);

    // Recognize planning requests
    if (normalized.find("organiza") != std::string::npos ||
        normalized.find("planeja") != std::string::npos ||
        normalized.find("planejar") != std::string::npos ||
        normalized.find("proposta para") != std::string::npos) {
        std::string horizon = "day";
        if (normalized.find("semana") != std::string::npos) horizon = "week";
        else if (normalized.find("manha") != std::string::npos) horizon = "morning";
        else if (normalized.find("tarde") != std::string::npos) horizon = "afternoon";

        return {
            .kind = "plan_request",
            .subject = horizon,
            .temporal = {},
            .precision = "horizon",
            .confidence = 1.0,
            .ambiguities = {},
            .source = name(),
        };
    }

    // Recognize morning intentions
    if (normalized.find("amanha de manha") != std::string::npos &&
        normalized.find("quero ") != std::string::npos) {
        return {
            .kind = "intention",
            .subject = extract_subject(request.utterance),
            .temporal = {.date_reference = "tomorrow", .period = "morning"},
            .precision = "date+period",
            .confidence = 1.0,
            .ambiguities = {},
            .source = name(),
        };
    }

    return {
        .kind = "unresolved_expression",
        .subject = {},
        .temporal = {},
        .precision = {},
        .confidence = 1.0,
        .ambiguities = {"unsupported_temporal_or_referential_expression"},
        .source = name(),
    };
}

PlanProposalCandidate DeterministicLanguage::propose_plan(const PlanRequest& request) {
    PlanProposalCandidate candidate;
    candidate.horizon = request.horizon.empty() ? "morning" : request.horizon;
    candidate.source = name();
    candidate.confidence = 1.0;

    auto ref = request.reference_time;
    if (ref.time_since_epoch().count() == 0) {
        ref = TimePoint{std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch())};
    }

    std::time_t raw = Clock::to_time_t(ref);
    std::tm local{};
    localtime_r(&raw, &local);

    if (candidate.horizon == "week") {
        candidate.summary = "Preparei uma proposta estruturada para tua semana.";
        local.tm_hour = 9;
        local.tm_min = 0;
        local.tm_sec = 0;

        TimePoint base{std::chrono::seconds{std::mktime(&local)}};
        std::vector<std::pair<std::string, std::string>> days = {
            {"Segunda — Pesquisa e foco profundo", "focus"},
            {"Terça — Alinhamentos e reuniões", "meeting"},
            {"Quarta — Redação e síntese", "focus"},
            {"Quinta — Campo e execuções externas", "focus"},
            {"Sexta — Revisão semanal e retrospectiva", "review"},
        };

        for (std::size_t i = 0; i < days.size(); ++i) {
            auto start = base + std::chrono::hours{static_cast<int64_t>(i * 24)};
            auto end = start + std::chrono::hours{4};
            candidate.blocks.push_back(PlanBlock{
                .title = days[i].first,
                .start = start,
                .end = end,
                .category = days[i].second,
                .intention_id = 0,
            });
        }
        candidate.points_of_attention.push_back("Terça-feira possui alta fragmentação de contexto.");
        candidate.points_of_attention.push_back("Quarta-feira possui uma janela limpa de 4h para trabalho contínuo.");
    } else {
        // Horizon: morning or day
        candidate.summary = "Preparei uma proposta para o teu período da manhã.";
        local.tm_hour = 9;
        local.tm_min = 0;
        local.tm_sec = 0;
        TimePoint block_start{std::chrono::seconds{std::mktime(&local)}};

        if (request.open_intentions.empty()) {
            candidate.blocks.push_back(PlanBlock{
                .title = "Bloco de foco prioritário",
                .start = block_start,
                .end = block_start + std::chrono::hours{2},
                .category = "focus",
                .intention_id = 0,
            });
            candidate.blocks.push_back(PlanBlock{
                .title = "Alinhamentos e revisão",
                .start = block_start + std::chrono::hours{2},
                .end = block_start + std::chrono::hours{3},
                .category = "review",
                .intention_id = 0,
            });
        } else {
            for (const auto& item : request.open_intentions) {
                candidate.blocks.push_back(PlanBlock{
                    .title = item.subject,
                    .start = block_start,
                    .end = block_start + std::chrono::hours{2},
                    .category = "focus",
                    .intention_id = item.id,
                });
                block_start = block_start + std::chrono::hours{2};
            }
            candidate.points_of_attention.push_back("Intenção alocada no início da manhã para proteger concentração.");
        }
    }

    return candidate;
}

FormulationResult DeterministicLanguage::formulate(const FormulationRequest& request) {
    if (request.decision == "SUGGEST") {
        return {"Você queria " + request.subject + ". Ainda faz sentido?", name()};
    }
    if (request.decision == "REMEMBER_MORNING") {
        return {"Certo. Amanhã de manhã eu trago isso de volta.", name()};
    }
    if (request.decision == "PLAN_PROPOSED") {
        return {"Preparei uma proposta de planejamento. Você pode ajustar ou aplicar diretamente.", name()};
    }
    return {{}, name()};
}

std::string DeterministicLanguage::name() const { return "deterministic-fallback"; }

std::unique_ptr<LanguageProvider> make_deterministic_language() {
    return std::make_unique<DeterministicLanguage>();
}

#ifndef LUME_HAS_LOCAL_LLM
std::unique_ptr<LanguageProvider> make_configured_language() {
    return make_deterministic_language();
}
#endif

}  // namespace lume
