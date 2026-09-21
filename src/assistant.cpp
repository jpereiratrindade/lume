#include "lume/assistant.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace lume {
namespace {

std::string trim(std::string value) {
    const auto space = [](unsigned char character) { return std::isspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    while (!value.empty() && (value.back() == '.' || value.back() == '!' || value.back() == '?')) {
        value.pop_back();
    }
    return value;
}

std::pair<TimePoint, TimePoint> tomorrow_morning(TimePoint now) {
    const std::time_t raw = Clock::to_time_t(now);
    std::tm local{};
    localtime_r(&raw, &local);
    ++local.tm_mday;
    local.tm_hour = 6;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    const auto start = TimePoint{std::chrono::seconds{std::mktime(&local)}};
    local.tm_hour = 12;
    local.tm_isdst = -1;
    const auto end = TimePoint{std::chrono::seconds{std::mktime(&local)}};
    return {start, end};
}

std::string json_escape(std::string_view value) {
    std::string result;
    for (const char byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
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

std::string quote(std::string_view value) { return "\"" + json_escape(value) + "\""; }

}  // namespace

Assistant::Assistant(Store store) : Assistant(std::move(store), make_deterministic_language()) {}

Assistant::Assistant(Store store, std::unique_ptr<LanguageProvider> language)
    : store_(std::move(store)), language_(std::move(language)) {
    if (!language_) throw std::invalid_argument("language provider must not be null");
}

Outcome Assistant::say(std::string_view expression, TimePoint now) {
    const auto clean = trim(std::string(expression));
    if (clean.empty()) return {"NO_OP", "Não ouvi nada para guardar.", "empty expression", false};

    auto state = store_.load();
    const auto expression_id = state.next_id++;
    state.expressions.push_back(Expression{expression_id, now, clean});

    ContextProjection context;
    for (const auto& item : state.intentions) {
        if (item.status == IntentionStatus::active) context.active_subject = item.subject;
        if (item.status == IntentionStatus::open) context.recent_open_subjects.push_back(item.subject);
    }
    const auto candidate = language_->interpret({clean, std::move(context)});

    // Language proposes. The core accepts only a known, complete, sufficiently confident shape.
    if (candidate.kind == "intention" && candidate.confidence >= 0.5 && !candidate.subject.empty() &&
        candidate.temporal.date_reference == "tomorrow" && candidate.temporal.period == "morning" &&
        candidate.precision == "date+period") {
        const auto [start, end] = tomorrow_morning(now);
        state.intentions.push_back(Intention{
            .id = state.next_id++,
            .expression_id = expression_id,
            .subject = candidate.subject,
            .window_start = start,
            .window_end = end,
            .precision = candidate.precision,
            .authority = "user",
            .interpretation_source = candidate.source.empty() ? language_->name() : candidate.source,
            .status = IntentionStatus::open,
            .last_interaction_at = std::nullopt,
        });
        store_.save(state);
        auto formulation = language_->formulate({"REMEMBER_MORNING", candidate.subject, {}});
        if (formulation.text.empty()) formulation = DeterministicLanguage{}.formulate(
            {"REMEMBER_MORNING", candidate.subject, {}});
        return {"REMEMBERED", formulation.text,
                "a expressão contém uma intenção e uma janela temporal parcial", true};
    }

    store_.save(state);
    return {"REMEMBERED", "Certo. Guardei exatamente como você disse.",
            "a expressão foi preservada; ainda não há base para decidir quando agir", true};
}

Outcome Assistant::observe(TimePoint now) {
    auto state = store_.load();
    auto candidate = std::find_if(state.intentions.begin(), state.intentions.end(), [&](const auto& item) {
        return item.status == IntentionStatus::open && now >= item.window_start && now <= item.window_end &&
               !item.last_interaction_at;
    });
    if (candidate == state.intentions.end()) {
        return {"NO_INTERACTION", "", "nenhuma intenção aberta pede atenção neste momento", false};
    }

    const auto reason = "você disse que queria " + candidate->subject +
                        "; a janela declarada está ativa e a intenção continua aberta";
    auto formulation = language_->formulate({"SUGGEST", candidate->subject, reason});
    if (formulation.text.empty()) {
        formulation.text = "Você deixou uma intenção aberta para este período: " + candidate->subject + ".";
        formulation.source = "core-fallback";
    }
    candidate->last_interaction_at = now;
    state.interactions.push_back(Interaction{
        .id = state.next_id++,
        .intention_id = candidate->id,
        .created_at = now,
        .message = formulation.text,
        .reason = reason,
        .decision = "INTERACT",
        .formulation_source = formulation.source,
    });
    store_.save(state);
    return {"INTERACT", formulation.text, reason, true};
}

Outcome Assistant::reply(std::string_view response, TimePoint now) {
    const auto clean = trim(std::string(response));
    if (clean.empty()) return {"NO_OP", "", "empty response", false};
    auto state = store_.load();

    auto target = state.intentions.end();
    for (auto iterator = state.intentions.begin(); iterator != state.intentions.end(); ++iterator) {
        if (iterator->status == IntentionStatus::open && iterator->last_interaction_at &&
            (target == state.intentions.end() || *iterator->last_interaction_at > *target->last_interaction_at)) {
            target = iterator;
        }
    }
    if (target == state.intentions.end()) {
        state.expressions.push_back(Expression{state.next_id++, now, clean});
        store_.save(state);
        return {"REMEMBERED", "Certo. Guardei tua resposta, mas não havia uma pergunta aberta.",
                "não existe interação pendente à qual associar a resposta", true};
    }

    state.expressions.push_back(Expression{state.next_id++, now, clean});
    const auto normalized = fold_portuguese(clean);
    if (normalized.find("daqui a uma hora") != std::string::npos) {
        target->window_start = now + std::chrono::hours{1};
        target->window_end = target->window_start + std::chrono::minutes{30};
        target->precision = "relative-hour";
        target->last_interaction_at.reset();
        store_.save(state);
        return {"DEFER", "Certo. Daqui a uma hora eu trago isso de volta.",
                "você adiou explicitamente a intenção por uma hora", true};
    }
    if (normalized == "sim" || normalized.starts_with("sim,")) {
        target->status = IntentionStatus::active;
        store_.save(state);
        return {"FOCUS_STARTED", "Certo. Considero isso teu foco agora.",
                "você confirmou a intenção", true};
    }
    if (normalized.find("ja resolvi") != std::string::npos ||
        normalized.find("conclui") != std::string::npos) {
        target->status = IntentionStatus::completed;
        store_.save(state);
        return {"COMPLETED", "Certo. Marco isso como resolvido.",
                "você informou que a intenção foi concluída", true};
    }
    if (normalized.find("nao precisa") != std::string::npos || normalized == "descarta") {
        target->status = IntentionStatus::dismissed;
        store_.save(state);
        return {"DISMISSED", "Tudo bem. Não vou trazer isso de volta.",
                "você retirou explicitamente a intenção", true};
    }
    if (normalized == "agora nao" || normalized == "nao agora") {
        target->window_start = now + std::chrono::hours{2};
        target->window_end = target->window_start + std::chrono::hours{2};
        target->precision = "assistant-default-deferral";
        target->last_interaction_at.reset();
        store_.save(state);
        return {"DEFER", "Tudo bem.", "você pediu para não retomar agora", true};
    }

    store_.save(state);
    return {"REMEMBERED", "Entendi. Preservei tua resposta sem presumir uma decisão.",
            "a resposta não autoriza uma mudança inequívoca no estado da intenção", true};
}

std::string Assistant::explain_last() const {
    const auto state = store_.load();
    if (state.interactions.empty()) return "Ainda não houve uma interação proativa para explicar.";
    return "Eu falei porque " + state.interactions.back().reason + ".";
}

std::string Assistant::language_name() const { return language_->name(); }

std::string Assistant::inspect() const {
    const auto state = store_.load();
    std::ostringstream output;
    output << "{\n  \"state_version\": 1,\n  \"expressions\": [";
    for (std::size_t index = 0; index < state.expressions.size(); ++index) {
        const auto& item = state.expressions[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << item.id
               << ", \"recorded_at\": " << quote(format_time(item.recorded_at))
               << ", \"text\": " << quote(item.text) << "}";
    }
    output << (state.expressions.empty() ? "" : "\n  ") << "],\n  \"intentions\": [";
    for (std::size_t index = 0; index < state.intentions.size(); ++index) {
        const auto& item = state.intentions[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << item.id
               << ", \"expression_id\": " << item.expression_id
               << ", \"subject\": " << quote(item.subject)
               << ", \"window_start\": " << quote(format_time(item.window_start))
               << ", \"window_end\": " << quote(format_time(item.window_end))
               << ", \"precision\": " << quote(item.precision)
               << ", \"authority\": " << quote(item.authority)
               << ", \"interpretation_source\": " << quote(item.interpretation_source)
               << ", \"status\": " << quote(to_string(item.status))
               << ", \"last_interaction_at\": ";
        if (item.last_interaction_at) output << quote(format_time(*item.last_interaction_at));
        else output << "null";
        output << "}";
    }
    output << (state.intentions.empty() ? "" : "\n  ") << "],\n  \"interactions\": [";
    for (std::size_t index = 0; index < state.interactions.size(); ++index) {
        const auto& item = state.interactions[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << item.id
               << ", \"intention_id\": " << item.intention_id
               << ", \"created_at\": " << quote(format_time(item.created_at))
               << ", \"decision\": " << quote(item.decision)
               << ", \"message\": " << quote(item.message)
               << ", \"reason\": " << quote(item.reason)
               << ", \"formulation_source\": " << quote(item.formulation_source) << "}";
    }
    output << (state.interactions.empty() ? "" : "\n  ") << "]\n}\n";
    return output.str();
}

}  // namespace lume
