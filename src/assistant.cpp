#include "lume/assistant.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

bool is_same_day(TimePoint a, TimePoint b) {
    const std::time_t raw_a = Clock::to_time_t(a);
    const std::time_t raw_b = Clock::to_time_t(b);
    std::tm tm_a{}, tm_b{};
    localtime_r(&raw_a, &tm_a);
    localtime_r(&raw_b, &tm_b);
    return tm_a.tm_year == tm_b.tm_year && tm_a.tm_yday == tm_b.tm_yday;
}

std::pair<int, int> get_hour_minute(TimePoint tp) {
    const std::time_t raw = Clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&raw, &tm);
    return {tm.tm_hour, tm.tm_min};
}

std::pair<int, int> parse_time_spec(std::string_view spec) {
    auto pos = spec.find(':');
    if (pos != std::string_view::npos && spec.substr(0, pos) == "daily_at") {
        spec = spec.substr(pos + 1);
    }
    auto colon = spec.find(':');
    if (colon == std::string_view::npos) return {-1, -1};
    try {
        int h = std::stoi(std::string(spec.substr(0, colon)));
        int m = std::stoi(std::string(spec.substr(colon + 1)));
        return {h, m};
    } catch (...) {
        return {-1, -1};
    }
}

std::string quote(std::string_view value) { return "\"" + json_escape(value) + "\""; }

std::string compute_intentions_digest(const std::vector<Intention>& intentions) {
    std::ostringstream ss;
    for (std::size_t i = 0; i < intentions.size(); ++i) {
        if (intentions[i].status != IntentionStatus::open) continue;
        if (ss.tellp() > 0) ss << ";";
        ss << intentions[i].id << ":" << intentions[i].subject << "@"
           << format_time(intentions[i].window_start) << "-" << format_time(intentions[i].window_end);
    }
    return ss.str();
}

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

std::optional<AttentionCandidate> Assistant::compute_attention_candidate(TimePoint now) const {
    const auto state = ledger_.project_state();
    for (const auto& item : state.intentions) {
        if (item.status != IntentionStatus::open) continue;

        TimePoint effective_start = item.allocated_plan_start.value_or(item.window_start);
        TimePoint effective_end = item.allocated_plan_end.value_or(item.window_end);

        if (now >= effective_start && now <= effective_end) {
            std::string reason = "A janela para \"" + item.subject + "\" está ativa agora";
            if (item.allocated_plan_start) {
                reason += " (conforme plano aprovado).";
            } else {
                reason += " (conforme declarado originalmente).";
            }
            return AttentionCandidate{
                .intention_id = item.id,
                .subject = item.subject,
                .window_start = effective_start,
                .window_end = effective_end,
                .relevance_reason = reason,
                .is_active_now = true,
            };
        }
    }
    return std::nullopt;
}

Outcome Assistant::say(std::string_view expression, TimePoint now) {
    const auto clean = trim(std::string(expression));
    if (clean.empty()) {
        return {"NO_OP", "Não ouvi nada para guardar.", "empty expression", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
    }

    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        const auto expression_id = state.next_id++;

        // 1. Record expression in immutable ledger with user_declared epistemic class
        ledger_.append(EventRecord{
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "user",
            .epistemic_class = EpistemicClass::user_declared,
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
                .sequence_number = 0,
                .recorded_at = now,
                .authority = "user",
                .epistemic_class = EpistemicClass::derived,
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
                    "a expressão contém uma intenção e uma janela temporal parcial", true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        return {"REMEMBERED", "Certo. Guardei exatamente como você disse.",
                "a expressão foi preservada; ainda não há base para decidir quando agir", true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
    });
}

Outcome Assistant::plan(std::string_view horizon_or_request, TimePoint now) {
    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        const auto records = ledger_.read_all();
        const std::uint64_t current_seq = records.empty() ? 0 : records.back().sequence_number;

        std::vector<Intention> open_items;
        for (const auto& item : state.intentions) {
            if (item.status == IntentionStatus::open) open_items.push_back(item);
        }

        std::string horizon = std::string(horizon_or_request);
        if (horizon.empty()) horizon = "morning";

        const std::string digest = compute_intentions_digest(open_items);

        auto candidate = language_->propose_plan(PlanRequest{
            .utterance = std::string(horizon_or_request),
            .horizon = horizon,
            .reference_time = now,
            .open_intentions = open_items,
        });

        // Core validation of plan proposal: remove invalid/overlapping blocks and ensure intention existence
        std::vector<PlanBlock> validated_blocks;
        TimePoint previous_end{};
        for (auto& block : candidate.blocks) {
            if (block.end <= block.start) continue; // discard negative or zero duration
            if (block.intention_id != 0) {
                bool exists = std::any_of(open_items.begin(), open_items.end(),
                                          [&](const auto& i) { return i.id == block.intention_id; });
                if (!exists) continue; // discard reference to non-existent intention
            }
            if (block.start < previous_end) {
                // Adjust start to avoid overlap
                auto duration = block.end - block.start;
                block.start = previous_end + std::chrono::minutes{15};
                block.end = block.start + duration;
            }
            previous_end = block.end;
            validated_blocks.push_back(std::move(block));
        }

        const auto plan_id = state.next_id++;
        PlanProposal proposal{
            .id = plan_id,
            .created_at = now,
            .horizon = candidate.horizon,
            .summary = candidate.summary,
            .blocks = std::move(validated_blocks),
            .points_of_attention = std::move(candidate.points_of_attention),
            .status = "draft",
            .source = candidate.source.empty() ? language_->name() : candidate.source,
            .as_of_sequence = current_seq,
            .basis_intentions_digest = digest,
        };

        ledger_.append(EventRecord{
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "core",
            .epistemic_class = EpistemicClass::proposed,
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
            .emitted_notifications = {},
            .attention_candidate = compute_attention_candidate(now),
        };
    });
}

Outcome Assistant::apply_plan(std::uint64_t plan_id, TimePoint now) {
    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        auto it = std::find_if(state.plan_proposals.begin(), state.plan_proposals.end(),
                               [&](const auto& p) { return p.id == plan_id; });
        if (it == state.plan_proposals.end()) {
            return {"NOT_FOUND", "Proposta de plano não encontrada.", "id inexistente", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        if (it->status == "applied") {
            return {"ALREADY_APPLIED", "Esta proposta já foi aplicada anteriormente e é terminal.", "plano já aplicado", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }
        if (it->status == "discarded") {
            return {"ALREADY_DISCARDED", "Esta proposta foi descartada anteriormente e não pode ser reativada.", "plano descartado", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        // Stale detection: verify that referenced open intentions are still valid and open
        for (const auto& block : it->blocks) {
            if (block.intention_id != 0) {
                auto intention_it = std::find_if(state.intentions.begin(), state.intentions.end(),
                                                 [&](const auto& i) { return i.id == block.intention_id; });
                if (intention_it == state.intentions.end() || intention_it->status != IntentionStatus::open) {
                    return {"PLAN_STALE", "O contexto das intenções mudou desde a criação do plano. Por favor, gere uma nova proposta.", "intenções foram modificadas ou concluídas", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
                }
            }
        }

        std::vector<EventRecord> batch;
        batch.push_back(EventRecord{
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "user",
            .epistemic_class = EpistemicClass::observed,
            .payload = EventPlanApplied{
                .plan_id = plan_id,
                .applied_at = now,
                .reason = "consentimento explícito do usuário",
            },
        });

        // Apply plan blocks to intentions without destroying original declaration
        for (const auto& block : it->blocks) {
            if (block.intention_id != 0) {
                batch.push_back(EventRecord{
                    .sequence_number = 0,
                    .recorded_at = now,
                    .authority = "user",
                    .epistemic_class = EpistemicClass::derived,
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

        ledger_.append_batch(std::move(batch));
        return {
            .decision = "PLAN_APPLIED",
            .message = "Proposta de planejamento aplicada. Os compromissos e intenções foram atualizados no ledger.",
            .reason = "consentimento explícito do usuário para aplicar o plano #" + std::to_string(plan_id),
            .changed = true,
            .type = "text",
            .plan_proposal = std::nullopt,
            .automation_proposal = std::nullopt,
            .emitted_notifications = {},
            .attention_candidate = compute_attention_candidate(now),
        };
    });
}

Outcome Assistant::discard_plan(std::uint64_t plan_id, TimePoint now) {
    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        auto it = std::find_if(state.plan_proposals.begin(), state.plan_proposals.end(),
                               [&](const auto& p) { return p.id == plan_id; });
        if (it == state.plan_proposals.end()) {
            return {"NOT_FOUND", "Proposta de plano não encontrada.", "id inexistente", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        if (it->status == "applied") {
            return {"ALREADY_APPLIED", "Esta proposta já foi aplicada e não pode ser descartada.", "plano já aplicado", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }
        if (it->status == "discarded") {
            return {"ALREADY_DISCARDED", "Esta proposta já foi descartada anteriormente.", "plano já descartado", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        ledger_.append(EventRecord{
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "user",
            .epistemic_class = EpistemicClass::observed,
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
            .emitted_notifications = {},
            .attention_candidate = compute_attention_candidate(now),
        };
    });
}

Outcome Assistant::create_automation(std::string_view title,
                                     std::string_view trigger_when,
                                     std::string_view condition_if,
                                     std::string_view action_then,
                                     std::string_view authority,
                                     TimePoint now) {
    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        const auto auto_id = state.next_id++;

        ledger_.append(EventRecord{
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "user",
            .epistemic_class = EpistemicClass::user_declared,
            .payload = EventAutomationCreated{
                .id = auto_id,
                .created_at = now,
                .title = std::string(title),
                .trigger_when = std::string(trigger_when),
                .condition_if = std::string(condition_if),
                .action_then = std::string(action_then),
                .authority = authority.empty() ? "suggest_only" : std::string(authority),
                .status = "active",
            },
        });

        return {
            .decision = "AUTOMATION_CREATED",
            .message = "Rotina de automação #" + std::to_string(auto_id) + " criada com sucesso.",
            .reason = "configuração de rotina pelo usuário",
            .changed = true,
            .type = "text",
            .plan_proposal = std::nullopt,
            .automation_proposal = AutomationProposal{
                .id = auto_id,
                .created_at = now,
                .title = std::string(title),
                .trigger_when = std::string(trigger_when),
                .condition_if = std::string(condition_if),
                .action_then = std::string(action_then),
                .authority = authority.empty() ? "suggest_only" : std::string(authority),
                .status = "active",
                .last_triggered_at = std::nullopt,
            },
            .emitted_notifications = {},
            .attention_candidate = compute_attention_candidate(now),
        };
    });
}

Outcome Assistant::toggle_automation(std::uint64_t automation_id, std::string_view new_status, TimePoint now) {
    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        auto it = std::find_if(state.automation_proposals.begin(), state.automation_proposals.end(),
                               [&](const auto& a) { return a.id == automation_id; });
        if (it == state.automation_proposals.end()) {
            return {"NOT_FOUND", "Automação não encontrada.", "id inexistente", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        std::string status_str = std::string(new_status);
        if (status_str != "active" && status_str != "paused" && status_str != "discarded") {
            return {"INVALID_STATUS", "Status inválido. Use active, paused ou discarded.", "status inválido", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        ledger_.append(EventRecord{
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "user",
            .epistemic_class = EpistemicClass::observed,
            .payload = EventAutomationStatusChanged{
                .automation_id = automation_id,
                .new_status = status_str,
                .reason = "ação explícita do usuário",
                .at = now,
            },
        });

        return {
            .decision = "AUTOMATION_STATUS_CHANGED",
            .message = "Automação #" + std::to_string(automation_id) + " alterada para " + status_str + ".",
            .reason = "atualização de status",
            .changed = true,
            .type = "text",
            .plan_proposal = std::nullopt,
            .automation_proposal = std::nullopt,
            .emitted_notifications = {},
            .attention_candidate = compute_attention_candidate(now),
        };
    });
}

Outcome Assistant::tick(TimePoint now) {
    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        std::vector<EventRecord> batch;
        std::vector<NotificationRecord> emitted;
        std::optional<PlanProposal> generated_plan;
        std::uint64_t next_id = state.next_id;

        const auto [now_hour, now_min] = get_hour_minute(now);

        for (const auto& auto_item : state.automation_proposals) {
            if (auto_item.status != "active") continue;

            bool should_trigger = false;
            std::string trigger_reason;

            // Check if triggered today
            if (auto_item.last_triggered_at && is_same_day(*auto_item.last_triggered_at, now)) {
                continue;
            }

            const auto [spec_hour, spec_min] = parse_time_spec(auto_item.trigger_when);
            if (spec_hour >= 0) {
                if (now_hour == spec_hour && now_min == spec_min) {
                    if (auto_item.condition_if == "has_open_intentions") {
                        bool has_open = std::any_of(state.intentions.begin(), state.intentions.end(),
                                                    [](const auto& i) { return i.status == IntentionStatus::open; });
                        if (has_open) {
                            should_trigger = true;
                            trigger_reason = "horário agendado com intenções abertas pendentes";
                        }
                    } else if (auto_item.condition_if == "always" || auto_item.condition_if.empty()) {
                        should_trigger = true;
                        trigger_reason = "horário programado";
                    }
                }
            } else if (auto_item.trigger_when == "on_overdue") {
                bool has_overdue = std::any_of(state.intentions.begin(), state.intentions.end(),
                                               [&](const auto& i) {
                                                   return i.status == IntentionStatus::open && i.window_end < now;
                                               });
                if (has_overdue) {
                    should_trigger = true;
                    trigger_reason = "intenções abertas ultrapassaram a janela planejada";
                }
            }

            if (should_trigger) {
                batch.push_back(EventRecord{
                    .sequence_number = 0,
                    .recorded_at = now,
                    .authority = "core",
                    .epistemic_class = EpistemicClass::observed,
                    .payload = EventAutomationTriggered{
                        .automation_id = auto_item.id,
                        .triggered_at = now,
                        .explanation = trigger_reason,
                    },
                });

                if (auto_item.action_then == "propose_daily_plan") {
                    std::vector<Intention> open_items;
                    for (const auto& item : state.intentions) {
                        if (item.status == IntentionStatus::open) open_items.push_back(item);
                    }
                    auto candidate = language_->propose_plan(PlanRequest{
                        .utterance = "Organizar manhã",
                        .horizon = "morning",
                        .reference_time = now,
                        .open_intentions = std::move(open_items),
                    });

                    PlanProposal proposal{
                        .id = next_id++,
                        .created_at = now,
                        .horizon = candidate.horizon,
                        .summary = candidate.summary,
                        .blocks = std::move(candidate.blocks),
                        .points_of_attention = std::move(candidate.points_of_attention),
                        .status = "draft",
                        .source = "automation:" + std::to_string(auto_item.id),
                        .as_of_sequence = 0,
                        .basis_intentions_digest = "",
                    };

                    batch.push_back(EventRecord{
                        .sequence_number = 0,
                        .recorded_at = now,
                        .authority = "core",
                        .epistemic_class = EpistemicClass::proposed,
                        .payload = EventPlanProposed{
                            .id = proposal.id,
                            .created_at = now,
                            .horizon = proposal.horizon,
                            .summary = proposal.summary,
                            .blocks = proposal.blocks,
                            .points_of_attention = proposal.points_of_attention,
                            .status = proposal.status,
                            .source = proposal.source,
                        },
                    });

                    const auto notif_id = next_id++;
                    NotificationRecord notif{
                        .id = notif_id,
                        .automation_id = auto_item.id,
                        .emitted_at = now,
                        .title = "Proposta de Organização do Dia",
                        .message = proposal.summary,
                        .action_type = "plan_proposal",
                        .reference_id = proposal.id,
                        .epistemic_class = EpistemicClass::observed,
                    };
                    batch.push_back(EventRecord{
                        .sequence_number = 0,
                        .recorded_at = now,
                        .authority = "core",
                        .epistemic_class = EpistemicClass::observed,
                        .payload = EventNotificationEmitted{
                            .id = notif.id,
                            .automation_id = notif.automation_id,
                            .emitted_at = notif.emitted_at,
                            .title = notif.title,
                            .message = notif.message,
                            .action_type = notif.action_type,
                            .reference_id = notif.reference_id,
                        },
                    });
                    emitted.push_back(notif);
                    generated_plan = proposal;
                } else if (auto_item.action_then == "ask_eod_review") {
                    std::size_t open_count = std::count_if(state.intentions.begin(), state.intentions.end(),
                                                          [](const auto& i) { return i.status == IntentionStatus::open; });
                    const auto notif_id = next_id++;
                    std::string msg = "Você tem " + std::to_string(open_count) + " intenção(ões) em aberto. Deseja revisá-las ou adiar para amanhã?";
                    NotificationRecord notif{
                        .id = notif_id,
                        .automation_id = auto_item.id,
                        .emitted_at = now,
                        .title = "Fechamento do Dia",
                        .message = msg,
                        .action_type = "eod_prompt",
                        .reference_id = std::nullopt,
                        .epistemic_class = EpistemicClass::observed,
                    };
                    batch.push_back(EventRecord{
                        .sequence_number = 0,
                        .recorded_at = now,
                        .authority = "core",
                        .epistemic_class = EpistemicClass::observed,
                        .payload = EventNotificationEmitted{
                            .id = notif.id,
                            .automation_id = notif.automation_id,
                            .emitted_at = notif.emitted_at,
                            .title = notif.title,
                            .message = notif.message,
                            .action_type = notif.action_type,
                            .reference_id = notif.reference_id,
                        },
                    });
                    emitted.push_back(notif);
                } else {
                    const auto notif_id = next_id++;
                    NotificationRecord notif{
                        .id = notif_id,
                        .automation_id = auto_item.id,
                        .emitted_at = now,
                        .title = auto_item.title.empty() ? "Lembrete do Lume" : auto_item.title,
                        .message = "Disparo da rotina: " + auto_item.action_then,
                        .action_type = "info",
                        .reference_id = std::nullopt,
                        .epistemic_class = EpistemicClass::observed,
                    };
                    batch.push_back(EventRecord{
                        .sequence_number = 0,
                        .recorded_at = now,
                        .authority = "core",
                        .epistemic_class = EpistemicClass::observed,
                        .payload = EventNotificationEmitted{
                            .id = notif.id,
                            .automation_id = notif.automation_id,
                            .emitted_at = notif.emitted_at,
                            .title = notif.title,
                            .message = notif.message,
                            .action_type = notif.action_type,
                            .reference_id = notif.reference_id,
                        },
                    });
                    emitted.push_back(notif);
                }
            }
        }

        if (!batch.empty()) {
            ledger_.append_batch(std::move(batch));
            return {
                .decision = "TICK_TRIGGERED",
                .message = std::to_string(emitted.size()) + " automação(ões) disparada(s).",
                .reason = "ciclo de avaliação do daemon",
                .changed = true,
                .type = generated_plan ? "plan_proposal" : "notifications",
                .plan_proposal = generated_plan,
                .automation_proposal = std::nullopt,
                .emitted_notifications = emitted,
                .attention_candidate = compute_attention_candidate(now),
            };
        }

        return {
            .decision = "TICK_IDLE",
            .message = "Nenhuma automação necessita de disparo no momento.",
            .reason = "condições de disparo não atingidas",
            .changed = false,
            .type = "text",
            .plan_proposal = std::nullopt,
            .automation_proposal = std::nullopt,
            .emitted_notifications = {},
            .attention_candidate = compute_attention_candidate(now),
        };
    });
}

Outcome Assistant::observe(TimePoint now) {
    return ledger_.with_exclusive_lock([&]() -> Outcome {
        auto state = ledger_.project_state();
        auto candidate = std::find_if(state.intentions.begin(), state.intentions.end(), [&](const auto& item) {
            TimePoint effective_start = item.allocated_plan_start.value_or(item.window_start);
            TimePoint effective_end = item.allocated_plan_end.value_or(item.window_end);
            return item.status == IntentionStatus::open && now >= effective_start && now <= effective_end &&
                   !item.last_interaction_at;
        });
        if (candidate == state.intentions.end()) {
            return {"NO_INTERACTION", "", "nenhuma intenção aberta pede atenção neste momento", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
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
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "core",
            .epistemic_class = EpistemicClass::observed,
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

        return {"INTERACT", formulation.text, reason, true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
    });
}

Outcome Assistant::reply(std::string_view response, TimePoint now) {
    const auto clean = trim(std::string(response));
    if (clean.empty()) return {"NO_OP", "", "empty response", false, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};

    return ledger_.with_exclusive_lock([&]() -> Outcome {
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
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "user",
            .epistemic_class = EpistemicClass::user_declared,
            .payload = EventExpressionRecorded{
                .id = reply_expr_id,
                .timestamp = now,
                .text = clean,
            },
        });

        if (target == state.intentions.end()) {
            return {"REMEMBERED", "Certo. Guardei tua resposta, mas não havia uma pergunta aberta.",
                    "não existe interação pendente à qual associar a resposta", true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        const auto normalized = fold_portuguese(clean);
        if (normalized.find("daqui a uma hora") != std::string::npos) {
            auto new_start = now + std::chrono::hours{1};
            auto new_end = new_start + std::chrono::minutes{30};
            ledger_.append(EventRecord{
                .sequence_number = 0,
                .recorded_at = now,
                .authority = "user",
                .epistemic_class = EpistemicClass::derived,
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
                    "você adiou explicitamente a intenção por uma hora", true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }
        if (normalized == "sim" || normalized.starts_with("sim,")) {
            ledger_.append(EventRecord{
                .sequence_number = 0,
                .recorded_at = now,
                .authority = "user",
                .epistemic_class = EpistemicClass::derived,
                .payload = EventIntentionStatusChanged{
                    .intention_id = target->id,
                    .new_status = IntentionStatus::active,
                    .reason = "confirmação do usuário para iniciar foco",
                    .at = now,
                },
            });
            return {"FOCUS_STARTED", "Certo. Considero isso teu foco agora.",
                    "você confirmou a intenção", true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }
        if (normalized.find("ja resolvi") != std::string::npos ||
            normalized.find("conclui") != std::string::npos) {
            ledger_.append(EventRecord{
                .sequence_number = 0,
                .recorded_at = now,
                .authority = "user",
                .epistemic_class = EpistemicClass::derived,
                .payload = EventIntentionStatusChanged{
                    .intention_id = target->id,
                    .new_status = IntentionStatus::completed,
                    .reason = "conclusão informada pelo usuário",
                    .at = now,
                },
            });
            return {"COMPLETED", "Certo. Marco isso como resolvido.",
                    "você informou que a intenção foi concluída", true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }
        if (normalized == "nao" || normalized.find("nao precisa") != std::string::npos ||
            normalized == "descarta") {
            ledger_.append(EventRecord{
                .sequence_number = 0,
                .recorded_at = now,
                .authority = "user",
                .epistemic_class = EpistemicClass::derived,
                .payload = EventIntentionStatusChanged{
                    .intention_id = target->id,
                    .new_status = IntentionStatus::dismissed,
                    .reason = "descarte informado pelo usuário",
                    .at = now,
                },
            });
            return {"DISMISSED", "Tudo bem. Não vou trazer isso de volta.",
                    "você retirou explicitamente a intenção", true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }
        if (normalized == "agora nao" || normalized == "nao agora") {
            const auto reason = "você recusou este momento sem definir outro";
            const auto message = "Tudo bem. Quando você quer que eu traga isso de volta?";
            const auto interaction_id = state.next_id++;
            ledger_.append(EventRecord{
                .sequence_number = 0,
                .recorded_at = now,
                .authority = "core",
                .epistemic_class = EpistemicClass::observed,
                .payload = EventInteractionRecorded{
                    .id = interaction_id,
                    .intention_id = target->id,
                    .timestamp = now,
                    .message = message,
                    .reason = reason,
                    .decision = "CLARIFY",
                    .formulation_source = "core",
                },
            });
            return {"CLARIFY", message, reason, true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
        }

        const auto reason = "a resposta não determina se a intenção deve começar, ser adiada ou encerrada";
        const auto message = "Não ficou claro se você quer fazer isso agora, adiar ou encerrar. O que prefere?";
        const auto interaction_id = state.next_id++;
        ledger_.append(EventRecord{
            .sequence_number = 0,
            .recorded_at = now,
            .authority = "core",
            .epistemic_class = EpistemicClass::observed,
            .payload = EventInteractionRecorded{
                .id = interaction_id,
                .intention_id = target->id,
                .timestamp = now,
                .message = message,
                .reason = reason,
                .decision = "CLARIFY",
                .formulation_source = "core",
            },
        });
        return {"CLARIFY", message, reason, true, "text", std::nullopt, std::nullopt, {}, compute_attention_candidate(now)};
    });
}

std::string Assistant::explain_last() const {
    const auto state = ledger_.project_state();
    if (state.interactions.empty()) return "Ainda não houve uma interação proativa para explicar.";
    return "Eu falei porque " + state.interactions.back().reason + ".";
}

std::string Assistant::language_name() const { return language_->name(); }

std::string Assistant::inspect() const {
    return inspect(TimePoint{std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch())});
}

std::string Assistant::inspect(TimePoint now) const {
    const auto state = ledger_.project_state();
    const auto records = ledger_.read_all();
    const auto attention = compute_attention_candidate(now);

    std::ostringstream output;
    output << "{\n  \"state_version\": 1,\n  \"ledger_events_count\": " << records.size() << ",\n";

    if (attention) {
        output << "  \"attention_candidate\": {\n"
               << "    \"intention_id\": " << attention->intention_id << ",\n"
               << "    \"subject\": " << quote(attention->subject) << ",\n"
               << "    \"window_start\": " << quote(format_time(attention->window_start)) << ",\n"
               << "    \"window_end\": " << quote(format_time(attention->window_end)) << ",\n"
               << "    \"relevance_reason\": " << quote(attention->relevance_reason) << ",\n"
               << "    \"is_active_now\": " << (attention->is_active_now ? "true" : "false") << "\n"
               << "  },\n";
    } else {
        output << "  \"attention_candidate\": null,\n";
    }

    output << "  \"expressions\": [";
    for (std::size_t index = 0; index < state.expressions.size(); ++index) {
        const auto& item = state.expressions[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << item.id
               << ", \"recorded_at\": " << quote(format_time(item.recorded_at))
               << ", \"text\": " << quote(item.text)
               << ", \"epistemic_class\": " << quote(to_string(item.epistemic_class)) << "}";
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
               << ", \"epistemic_class\": " << quote(to_string(item.epistemic_class))
               << ", \"interpretation_source\": " << quote(item.interpretation_source)
               << ", \"status\": " << quote(to_string(item.status))
               << ", \"allocated_plan_start\": " << (item.allocated_plan_start ? quote(format_time(*item.allocated_plan_start)) : "null")
               << ", \"allocated_plan_end\": " << (item.allocated_plan_end ? quote(format_time(*item.allocated_plan_end)) : "null")
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
               << ", \"epistemic_class\": " << quote(to_string(item.epistemic_class))
               << ", \"formulation_source\": " << quote(item.formulation_source) << "}";
    }
    output << (state.interactions.empty() ? "" : "\n  ") << "],\n  \"plan_proposals\": [";
    for (std::size_t index = 0; index < state.plan_proposals.size(); ++index) {
        const auto& plan = state.plan_proposals[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << plan.id
               << ", \"created_at\": " << quote(format_time(plan.created_at))
               << ", \"horizon\": " << quote(plan.horizon)
               << ", \"summary\": " << quote(plan.summary)
               << ", \"status\": " << quote(plan.status)
               << ", \"source\": " << quote(plan.source)
               << ", \"as_of_sequence\": " << plan.as_of_sequence
               << ", \"basis_intentions_digest\": " << quote(plan.basis_intentions_digest)
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
    output << (state.plan_proposals.empty() ? "" : "\n  ") << "],\n  \"automation_proposals\": [";
    for (std::size_t index = 0; index < state.automation_proposals.size(); ++index) {
        const auto& a = state.automation_proposals[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << a.id
               << ", \"title\": " << quote(a.title)
               << ", \"trigger_when\": " << quote(a.trigger_when)
               << ", \"condition_if\": " << quote(a.condition_if)
               << ", \"action_then\": " << quote(a.action_then)
               << ", \"authority\": " << quote(a.authority)
               << ", \"status\": " << quote(a.status)
               << ", \"last_triggered_at\": " << (a.last_triggered_at ? quote(format_time(*a.last_triggered_at)) : "null") << "}";
    }
    output << (state.automation_proposals.empty() ? "" : "\n  ") << "],\n  \"notifications\": [";
    for (std::size_t index = 0; index < state.notifications.size(); ++index) {
        const auto& n = state.notifications[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"id\": " << n.id
               << ", \"automation_id\": " << n.automation_id
               << ", \"emitted_at\": " << quote(format_time(n.emitted_at))
               << ", \"title\": " << quote(n.title)
               << ", \"message\": " << quote(n.message)
               << ", \"action_type\": " << quote(n.action_type)
               << ", \"reference_id\": " << (n.reference_id ? std::to_string(*n.reference_id) : "null") << "}";
    }
    output << (state.notifications.empty() ? "" : "\n  ") << "]\n}\n";
    return output.str();
}

}  // namespace lume
