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

FormulationResult DeterministicLanguage::formulate(const FormulationRequest& request) {
    if (request.decision == "SUGGEST") {
        return {"Você queria " + request.subject + ". Ainda faz sentido?", name()};
    }
    if (request.decision == "REMEMBER_MORNING") {
        return {"Certo. Amanhã de manhã eu trago isso de volta.", name()};
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
