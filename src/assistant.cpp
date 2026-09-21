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

Assistant::Assistant(Ledger ledger) : Assistant(std::move(ledger), make_deterministic_language()) {}

Assistant::Assistant(Ledger ledger, std::unique_ptr<LanguageProvider> language)
    : ledger_(std::move(ledger)), language_(std::move(language)) {
    if (!language_) throw std::invalid_argument("language provider must not be null");
}

Assistant::Assistant(Store store) : Assistant(Ledger{store.path()}, make_deterministic_language()) {}

Assistant::Assistant(Store store, std::unique_ptr<LanguageProvider> language)
    : Assistant(Ledger{store.path()}, std::move(language)) {}

const Ledger& Assistant::ledger() const noexcept { return ledger_; }

Outcome Assistant::say(std::string_view expression, TimePoint now) {
    const auto clean = trim(std::string(expression));
    if (clean.empty()) return {"NO_OP", "Não ouvi nada para guardar.", "empty expression", false, "text", std::nullopt, std::nullopt};

    auto state = ledger_.project_state();
    const auto expression_id = state.next_id++;

    // 1. Record expression in immutable ledger
    ledger_.append(EventRecord{
        .sequence_number = state.next_id++,
        .recorded_at = now,
        .authority = "user",
        .payload = EventExpressionRecorded{
            .id = expression_id,
            .timestamp = now,
            .text = clean,
        },
    });

    ContextProjection context;
    for (const auto& item : state.intentions) {
        if (item.status == IntentionStatus::active) context.active_subject = item.subject;
        if (item.status == IntentionStatus::open) {
            context.recent_open_subjects.push_back(item.subject);
            context.open_intentions.push_back(item);
        }
    }

    const auto candidate = language_->interpret({clean, std::move(context)});

    // If it's a request to plan / organize
    if (candidate.kind == "plan_request") {
        return plan(candidate.subject, now);
    }

    // Language proposes. The core accepts only a known, complete, sufficiently confident shape.
    if (candidate.kind == "intention" && candidate.confidence >= 0.5 && !candidate.subject.empty() &&
        candidate.temporal.date_reference == "tomorrow" && candidate.temporal.period == "morning" &&
        candidate.precision == "date+period") {
        const auto [start, end] = tomorrow_morning(now);
        const auto intention_id = state.next_id++;

        ledger_.append(EventRecord{
            .sequence_number = state.next_id++,
            .recorded_at = now,
            .authority = "user",
            .payload = EventIntentionDerived{
                .id = intention_id,
                .expression_id = expression_id,
                .subject = candidate.subject,
                .window_start = start,
                .window_end = end,
                .precision = candidate.precision,
                .authority = "user",
                .interpretation_source = candidate.source.empty() ? language_->name() : candidate.source,
            },
        });

        auto formulation = language_->formulate({"REMEMBER_MORNING", candidate.subject, {}});
        if (formulation.text.empty()) formulation = DeterministicLanguage{}.formulate(
            {"REMEMBER_MORNING", candidate.subject, {}});
        return {"REMEMBERED", formulation.text,
                "a expressão contém uma intenção e uma janela temporal parcial", true, "text", std::nullopt, std::nullopt};
    }

    return {"REMEMBERED", "Certo. Guardei exatamente como você disse.",
            "a expressão foi preservada; ainda não há base para decidir quando agir", true, "text", std::nullopt, std::nullopt};
}

Outcome Assistant::plan(std::string_view horizon_or_request, TimePoint now) {
    auto state = ledger_.project_state();

    std::vector<Intention> open_items;
    for (const auto& item : state.intentions) {
        if (item.status == IntentionStatus::open) open_items.push_back(item);
    }

    std::string horizon = std::string(horizon_or_request);
    if (horizon.empty()) horizon = "morning";

    auto candidate = language_->propose_plan(PlanRequest{
        .utterance = std::string(horizon_or_request),
        .horizon = horizon,
        .reference_time = now,
        .open_intentions = std::move(open_items),
    });

    const auto plan_id = state.next_id++;
    PlanProposal proposal{
        .id = plan_id,
        .created_at = now,
        .horizon = candidate.horizon,
        .summary = candidate.summary,
        .blocks = std::move(candidate.blocks),
        .points_of_attention = std::move(candidate.points_of_attention),
        .status = "draft",
        .source = candidate.source.empty() ? language_->name() : candidate.source,
    };

    ledger_.append(EventRecord{
        .sequence_number = state.next_id++,
        .recorded_at = now,
        .authority = "core",
        .payload = EventPlanProposed{
            .id = proposal.id,
            .created_at = proposal.created_at,
            .horizon = proposal.horizon,
            .summary = proposal.summary,
            .blocks = proposal.blocks,
            .points_of_attention = proposal.points_of_attention,
            .status = proposal.status,
            .source = proposal.source,
        },
    });

    return {
        .decision = "PLAN_PROPOSED",
        .message = proposal.summary,
        .reason = "proposta de organização estruturada em estado de rascunho (aguarda confirmação)",
        .changed = true,
        .type = "plan_proposal",
        .plan_proposal = std::move(proposal),
        .automation_proposal = std::nullopt,
    };
}

Outcome Assistant::apply_plan(std::uint64_t plan_id, TimePoint now) {
    auto state = ledger_.project_state();
    auto it = std::find_if(state.plan_proposals.begin(), state.plan_proposals.end(),
                           [&](const auto& p) { return p.id == plan_id; });
    if (it == state.plan_proposals.end()) {
        return {"NOT_FOUND", "Proposta de plano não encontrada.", "id inexistente", false, "text", std::nullopt, std::nullopt};
    }

    std::vector<EventRecord> batch;
    batch.push_back(EventRecord{
        .sequence_number = state.next_id++,
        .recorded_at = now,
        .authority = "user",
        .payload = EventPlanApplied{
            .plan_id = plan_id,
            .applied_at = now,
            .reason = "consentimento explícito do usuário",
        },
    });

    // Apply plan blocks to intentions
    for (const auto& block : it->blocks) {
        if (block.intention_id != 0) {
            batch.push_back(EventRecord{
                .sequence_number = state.next_id++,
                .recorded_at = now,
                .authority = "user",
                .payload = EventIntentionDeferred{
                    .intention_id = block.intention_id,
                    .new_start = block.start,
                    .new_end = block.end,
                    .precision = "plan_slot",
                    .reason = "alocado no plano #" + std::to_string(plan_id),
                    .at = now,
                },
            });
        }
    }

    ledger_.append_batch(batch);
    return {
        .decision = "PLAN_APPLIED",
        .message = "Proposta de planejamento aplicada. Os compromissos e intenções foram atualizados no ledger.",
        .reason = "consentimento explícito do usuário para aplicar o plano #" + std::to_string(plan_id),
        .changed = true,
        .type = "text",
        .plan_proposal = std::nullopt,
        .automation_proposal = std::nullopt,
    };
}

Outcome Assistant::discard_plan(std::uint64_t plan_id, TimePoint now) {
    auto state = ledger_.project_state();
    auto it = std::find_if(state.plan_proposals.begin(), state.plan_proposals.end(),
                           [&](const auto& p) { return p.id == plan_id; });
    if (it == state.plan_proposals.end()) {
        return {"NOT_FOUND", "Proposta de plano não encontrada.", "id inexistente", false, "text", std::nullopt, std::nullopt};
    }

    ledger_.append(EventRecord{
        .sequence_number = state.next_id++,
        .recorded_at = now,
        .authority = "user",
        .payload = EventPlanDiscarded{
            .plan_id = plan_id,
            .discarded_at = now,
            .reason = "descarte explícito pelo usuário",
        },
    });

    return {
        .decision = "PLAN_DISCARDED",
        .message = "Proposta descartada. Nenhuma alteração foi realizada nas tuas intenções.",
        .reason = "usuário optou por descartar o plano #" + std::to_string(plan_id),
        .changed = true,
        .type = "text",
        .plan_proposal = std::nullopt,
        .automation_proposal = std::nullopt,
    };
}

Outcome Assistant::observe(TimePoint now) {
    auto state = ledger_.project_state();
    auto candidate = std::find_if(state.intentions.begin(), state.intentions.end(), [&](const auto& item) {
        return item.status == IntentionStatus::open && now >= item.window_start && now <= item.window_end &&
               !item.last_interaction_at;
    });
    if (candidate == state.intentions.end()) {
        return {"NO_INTERACTION", "", "nenhuma intenção aberta pede atenção neste momento", false, "text", std::nullopt, std::nullopt};
    }

    const auto reason = "você disse que queria " + candidate->subject +
                        "; a janela declarada está ativa e a intenção continua aberta";
    auto formulation = language_->formulate({"SUGGEST", candidate->subject, reason});
    if (formulation.text.empty()) {
        formulation.text = "Você deixou uma intenção aberta para este período: " + candidate->subject + ".";
        formulation.source = "core-fallback";
    }

    const auto interaction_id = state.next_id++;
    ledger_.append(EventRecord{
        .sequence_number = state.next_id++,
        .recorded_at = now,
        .authority = "core",
        .payload = EventInteractionRecorded{
            .id = interaction_id,
            .intention_id = candidate->id,
            .timestamp = now,
            .message = formulation.text,
            .reason = reason,
            .decision = "INTERACT",
            .formulation_source = formulation.source,
        },
    });

    return {"INTERACT", formulation.text, reason, true, "text", std::nullopt, std::nullopt};
}

Outcome Assistant::reply(std::string_view response, TimePoint now) {
    const auto clean = trim(std::string(response));
    if (clean.empty()) return {"NO_OP", "", "empty response", false, "text", std::nullopt, std::nullopt};
    auto state = ledger_.project_state();

    auto target = state.intentions.end();
    for (auto iterator = state.intentions.begin(); iterator != state.intentions.end(); ++iterator) {
        if (iterator->status == IntentionStatus::open && iterator->last_interaction_at &&
            (target == state.intentions.end() || *iterator->last_interaction_at > *target->last_interaction_at)) {
            target = iterator;
        }
    }

    // Always record user reply expression
    const auto reply_expr_id = state.next_id++;
    ledger_.append(EventRecord{
        .sequence_number = state.next_id++,
        .recorded_at = now,
        .authority = "user",
        .payload = EventExpressionRecorded{
            .id = reply_expr_id,
            .timestamp = now,
            .text = clean,
        },
    });

    if (target == state.intentions.end()) {
        return {"REMEMBERED", "Certo. Guardei tua resposta, mas não havia uma pergunta aberta.",
                "não existe interação pendente à qual associar a resposta", true, "text", std::nullopt, std::nullopt};
    }

    const auto normalized = fold_portuguese(clean);
    if (normalized.find("daqui a uma hora") != std::string::npos) {
        auto new_start = now + std::chrono::hours{1};
        auto new_end = new_start + std::chrono::minutes{30};
        ledger_.append(EventRecord{
            .sequence_number = state.next_id++,
            .recorded_at = now,
            .authority = "user",
            .payload = EventIntentionDeferred{
                .intention_id = target->id,
                .new_start = new_start,
                .new_end = new_end,
                .precision = "relative-hour",
                .reason = "adiamento solicitado pelo usuário",
                .at = now,
            },
        });
        return {"DEFER", "Certo. Daqui a uma hora eu trago isso de volta.",
                "você adiou explicitamente a intenção por uma hora", true, "text", std::nullopt, std::nullopt};
    }
    if (normalized == "sim" || normalized.starts_with("sim,")) {
        ledger_.append(EventRecord{
            .sequence_number = state.next_id++,
            .recorded_at = now,
            .authority = "user",
            .payload = EventIntentionStatusChanged{
                .intention_id = target->id,
                .new_status = IntentionStatus::active,
                .reason = "confirmação do usuário para iniciar foco",
                .at = now,
            },
        });
        return {"FOCUS_STARTED", "Certo. Considero isso teu foco agora.",
                "você confirmou a intenção", true, "text", std::nullopt, std::nullopt};
    }
    if (normalized.find("ja resolvi") != std::string::npos ||
        normalized.find("conclui") != std::string::npos) {
        ledger_.append(EventRecord{
            .sequence_number = state.next_id++,
            .recorded_at = now,
            .authority = "user",
            .payload = EventIntentionStatusChanged{
                .intention_id = target->id,
                .new_status = IntentionStatus::completed,
                .reason = "conclusão informada pelo usuário",
                .at = now,
            },
        });
        return {"COMPLETED", "Certo. Marco isso como resolvido.",
                "você informou que a intenção foi concluída", true, "text", std::nullopt, std::nullopt};
    }
    if (normalized == "nao" || normalized.find("nao precisa") != std::string::npos ||
        normalized == "descarta") {
        ledger_.append(EventRecord{
            .sequence_number = state.next_id++,
            .recorded_at = now,
            .authority = "user",
            .payload = EventIntentionStatusChanged{
                .intention_id = target->id,
                .new_status = IntentionStatus::dismissed,
                .reason = "descarte informado pelo usuário",
                .at = now,
            },
        });
        return {"DISMISSED", "Tudo bem. Não vou trazer isso de volta.",
                "você retirou explicitamente a intenção", true, "text", std::nullopt, std::nullopt};
    }
    if (normalized == "agora nao" || normalized == "nao agora") {
        const auto reason = "você recusou este momento sem definir outro";
        const auto message = "Tudo bem. Quando você quer que eu traga isso de volta?";
        ledger_.append(EventRecord{
            .sequence_number = state.next_id++,
            .recorded_at = now,
            .authority = "core",
            .payload = EventInteractionRecorded{
                .id = state.next_id++,
                .intention_id = target->id,
                .timestamp = now,
                .message = message,
                .reason = reason,
                .decision = "CLARIFY",
                .formulation_source = "core",
            },
        });
        return {"CLARIFY", message, reason, true, "text", std::nullopt, std::nullopt};
    }

    const auto reason = "a resposta não determina se a intenção deve começar, ser adiada ou encerrada";
    const auto message = "Não ficou claro se você quer fazer isso agora, adiar ou encerrar. O que prefere?";
    ledger_.append(EventRecord{
        .sequence_number = state.next_id++,
        .recorded_at = now,
        .authority = "core",
        .payload = EventInteractionRecorded{
            .id = state.next_id++,
            .intention_id = target->id,
            .timestamp = now,
            .message = message,
            .reason = reason,
            .decision = "CLARIFY",
            .formulation_source = "core",
        },
    });
    return {"CLARIFY", message, reason, true, "text", std::nullopt, std::nullopt};
}

std::string Assistant::explain_last() const {
    const auto state = ledger_.project_state();
    if (state.interactions.empty()) return "Ainda não houve uma interação proativa para explicar.";
    return "Eu falei porque " + state.interactions.back().reason + ".";
}

std::string Assistant::language_name() const { return language_->name(); }

std::string Assistant::inspect() const {
    const auto state = ledger_.project_state();
    const auto records = ledger_.read_all();

    std::ostringstream output;
    output << "{\n  \"state_version\": 1,\n  \"ledger_events_count\": " << records.size()
           << ",\n  \"expressions\": [";
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
    output << (state.interactions.empty() ? "" : "\n  ") << "],\n  \"plan_proposals\": [";
    for (std::size_t index = 0; index < state.plan_proposals.size(); ++index) {
        const auto& plan = state.plan_proposals[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << plan.id
               << ", \"horizon\": " << quote(plan.horizon)
               << ", \"summary\": " << quote(plan.summary)
               << ", \"status\": " << quote(plan.status)
               << ", \"source\": " << quote(plan.source)
               << ", \"blocks\": [";
        for (std::size_t bi = 0; bi < plan.blocks.size(); ++bi) {
            const auto& b = plan.blocks[bi];
            output << (bi == 0 ? "" : ", ")
                   << "{\"title\": " << quote(b.title)
                   << ", \"start\": " << quote(format_time(b.start))
                   << ", \"end\": " << quote(format_time(b.end))
                   << ", \"category\": " << quote(b.category)
                   << ", \"intention_id\": " << b.intention_id << "}";
        }
        output << "], \"points_of_attention\": [";
        for (std::size_t pi = 0; pi < plan.points_of_attention.size(); ++pi) {
            output << (pi == 0 ? "" : ", ") << quote(plan.points_of_attention[pi]);
        }
        output << "]}";
    }
    output << (state.plan_proposals.empty() ? "" : "\n  ") << "]\n}\n";
    return output.str();
}

}  // namespace lume
