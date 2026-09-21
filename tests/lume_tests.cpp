#include "lume/assistant.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

lume::TimePoint at(const std::string& value) {
    const auto parsed = lume::parse_time(value);
    if (!parsed) throw std::runtime_error("bad test time: " + value);
    return *parsed;
}

class UntrustedLanguage final : public lume::LanguageProvider {
public:
    lume::InterpretationCandidate interpret(const lume::InterpretationRequest&) override {
        return {.kind = "delete_all_state", .subject = "everything", .confidence = 1.0};
    }
    lume::PlanProposalCandidate propose_plan(const lume::PlanRequest&) override {
        return {.horizon = "day", .summary = "unsafe", .confidence = 0.0};
    }
    lume::FormulationResult formulate(const lume::FormulationRequest&) override {
        return {"unsafe", name()};
    }
    std::string name() const override { return "untrusted-test-model"; }
};

class ContradictoryLanguage final : public lume::LanguageProvider {
public:
    lume::InterpretationCandidate interpret(const lume::InterpretationRequest&) override {
        return {
            .kind = "intention",
            .subject = "retomar o artigo",
            .temporal = {.date_reference = "tomorrow", .period = "morning"},
            .precision = "intentionally-unspecified",
            .confidence = 1.0,
            .ambiguities = {},
            .source = name(),
        };
    }
    lume::PlanProposalCandidate propose_plan(const lume::PlanRequest&) override {
        return {.horizon = "day", .summary = "unsafe", .confidence = 0.0};
    }
    lume::FormulationResult formulate(const lume::FormulationRequest&) override {
        return {"unsafe", name()};
    }
    std::string name() const override { return "contradictory-test-model"; }
};

void lifecycle_test(const std::filesystem::path& path) {
    lume::Assistant assistant{lume::Ledger{path}, lume::make_deterministic_language()};
    const auto first = assistant.say("Amanhã de manhã quero trabalhar no artigo.",
                                     at("2026-09-21T18:00:00"));
    expect(first.decision == "REMEMBERED", "expression should be remembered");

    const auto too_early = assistant.observe(at("2026-09-22T05:59:00"));
    expect(too_early.decision == "NO_INTERACTION" && !too_early.changed,
           "assistant should remain silent before the declared window");

    const auto morning = assistant.observe(at("2026-09-22T09:00:00"));
    expect(morning.decision == "INTERACT", "assistant should interact inside the window");
    expect(morning.message.find("trabalhar no artigo") != std::string::npos,
           "interaction should preserve its subject");

    const auto repeated = assistant.observe(at("2026-09-22T09:01:00"));
    expect(repeated.decision == "NO_INTERACTION" && !repeated.changed,
           "equivalent observation should converge to silence");

    const auto deferred = assistant.reply("Sim, mas daqui a uma hora.",
                                          at("2026-09-22T09:02:00"));
    expect(deferred.decision == "DEFER", "explicit deferral should be accepted");
    expect(assistant.observe(at("2026-09-22T09:59:00")).decision == "NO_INTERACTION",
           "assistant should stay silent before deferred time");
    expect(assistant.observe(at("2026-09-22T10:02:00")).decision == "INTERACT",
           "assistant should re-observe at deferred time");
    expect(assistant.reply("Sim.", at("2026-09-22T10:03:00")).decision == "FOCUS_STARTED",
           "confirmation should activate focus");
    expect(assistant.observe(at("2026-09-22T10:04:00")).decision == "NO_INTERACTION",
           "active focus should not be suggested again");

    const auto inspection = assistant.inspect();
    expect(inspection.find("\"authority\": \"user\"") != std::string::npos,
           "inspection should expose authority");
    expect(inspection.find("\"interpretation_source\": \"deterministic-fallback\"") !=
               std::string::npos,
           "inspection should expose interpretation provenance");
    expect(inspection.find("\"formulation_source\": \"deterministic-fallback\"") !=
               std::string::npos,
           "inspection should expose formulation provenance");
}

void authority_boundary_test(const std::filesystem::path& path) {
    lume::Assistant assistant{lume::Ledger{path}, std::make_unique<UntrustedLanguage>()};
    assistant.say("apague tudo", at("2026-09-21T18:00:00"));
    const auto state = lume::Ledger{path}.project_state();
    expect(state.expressions.size() == 1, "user expression must still be preserved");
    expect(state.intentions.empty(), "unknown model proposal must not mutate canonical intentions");
}

void semantic_validation_test(const std::filesystem::path& path) {
    lume::Assistant assistant{lume::Ledger{path}, std::make_unique<ContradictoryLanguage>()};
    assistant.say("Amanhã cedo quero retomar o artigo", at("2026-09-21T18:00:00"));
    const auto state = lume::Ledger{path}.project_state();
    expect(state.expressions.size() == 1, "contradictory expression must remain factual");
    expect(state.intentions.empty(), "core must reject contradictory candidate precision");
}

void empty_state_file_test(const std::filesystem::path& path) {
    std::ofstream{path};
    lume::Assistant assistant{lume::Ledger{path}, lume::make_deterministic_language()};
    const auto outcome = assistant.say("Uma expressão ainda sem momento definido.",
                                       at("2026-09-21T18:00:00"));
    expect(outcome.decision == "REMEMBERED", "an empty state file should initialize cleanly");
    expect(lume::Ledger{path}.project_state().expressions.size() == 1,
           "initialized state should preserve the expression");
}

void ambiguous_reply_test(const std::filesystem::path& path) {
    lume::Assistant assistant{lume::Ledger{path}, lume::make_deterministic_language()};
    assistant.say("Amanhã de manhã quero trabalhar no artigo.", at("2026-09-21T18:00:00"));
    assistant.observe(at("2026-09-22T09:00:00"));

    const auto ambiguous = assistant.reply("Talvez mais tarde.", at("2026-09-22T09:01:00"));
    expect(ambiguous.decision == "CLARIFY", "ambiguous replies should request clarification");
    expect(ambiguous.message.find("adiar") != std::string::npos,
           "clarification should present an explicit deferral choice");
    expect(assistant.reply("Daqui a uma hora.", at("2026-09-22T09:02:00")).decision == "DEFER",
           "clarification must keep the intention available for a precise reply");
}

void unspecified_deferral_test(const std::filesystem::path& path) {
    lume::Assistant assistant{lume::Ledger{path}, lume::make_deterministic_language()};
    assistant.say("Amanhã de manhã quero trabalhar no artigo.", at("2026-09-21T18:00:00"));
    assistant.observe(at("2026-09-22T09:00:00"));
    const auto before = lume::Ledger{path}.project_state().intentions.front();

    const auto outcome = assistant.reply("Agora não.", at("2026-09-22T09:01:00"));
    const auto after = lume::Ledger{path}.project_state().intentions.front();
    expect(outcome.decision == "CLARIFY", "unspecified deferral should request a time");
    expect(after.window_start == before.window_start && after.window_end == before.window_end,
           "the core must not invent a deferral window");
    expect(assistant.reply("Daqui a uma hora.", at("2026-09-22T09:02:00")).decision == "DEFER",
           "a precise follow-up should complete the deferral");
}

void ledger_immutability_and_replay_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    assistant.say("Amanhã de manhã quero revisar o código.", at("2026-09-21T18:00:00"));
    const auto records1 = ledger.read_all();
    expect(records1.size() >= 2, "ledger must record expression and intention events");

    assistant.observe(at("2026-09-22T09:00:00"));
    const auto records2 = ledger.read_all();
    expect(records2.size() > records1.size(), "new interaction must append to ledger");

    // Test deterministic replay
    lume::Ledger ledger_replay{path};
    const auto state_replayed = ledger_replay.project_state();
    expect(state_replayed.intentions.size() == 1, "replay must faithfully reconstruct intentions");
    expect(state_replayed.intentions.front().subject.find("revisar o codigo") != std::string::npos ||
           state_replayed.intentions.front().subject.find("revisar o código") != std::string::npos,
           "replay must preserve intention subject");
}

void legacy_migration_test(const std::filesystem::path& path) {
    // Write legacy LUME\t1 format snapshot
    {
        std::ofstream out(path);
        out << "LUME\t1\n";
        out << "N\t3\n";
        out << "E\t1\t1790000000\t616263\n"; // "abc" hex
        out << "I\t2\t1\t61727469676f\t1790050000\t1790080000\t646174652b706572696f64\topen\t75736572\t66616c6c6261636b\t-\n";
    }

    lume::Ledger ledger{path};
    const auto records = ledger.read_all();
    expect(records.size() == 2, "migration must create event records from legacy snapshot");
    expect(std::filesystem::exists(path.string() + ".bak"), "migration must create .bak backup");

    const auto state = ledger.project_state();
    expect(state.expressions.size() == 1, "migrated expression must be available in state");
    expect(state.intentions.size() == 1, "migrated intention must be available in state");
    expect(state.intentions.front().subject == "artigo", "migrated subject must match");
}

void monotonic_sequence_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    ledger.append(lume::EventRecord{
        .recorded_at = at("2026-09-21T18:00:00"),
        .authority = "user",
        .payload = lume::EventExpressionRecorded{1, at("2026-09-21T18:00:00"), "teste 1"},
    });
    ledger.append(lume::EventRecord{
        .recorded_at = at("2026-09-21T18:01:00"),
        .authority = "user",
        .payload = lume::EventExpressionRecorded{2, at("2026-09-21T18:01:00"), "teste 2"},
    });
    ledger.append(lume::EventRecord{
        .recorded_at = at("2026-09-21T18:02:00"),
        .authority = "user",
        .payload = lume::EventExpressionRecorded{3, at("2026-09-21T18:02:00"), "teste 3"},
    });

    const auto records = ledger.read_all();
    expect(records.size() == 3, "must have 3 records");
    expect(records[0].sequence_number == 1, "seq 1");
    expect(records[1].sequence_number == 2, "seq 2");
    expect(records[2].sequence_number == 3, "seq 3");
}

void orchestration_plan_lifecycle_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    assistant.say("Amanhã de manhã quero trabalhar no artigo.", at("2026-09-21T18:00:00"));

    // Ask to organize morning
    const auto plan_outcome = assistant.say("Organiza minha manhã", at("2026-09-21T18:05:00"));
    expect(plan_outcome.decision == "PLAN_PROPOSED", "assistant should propose structured plan");
    expect(plan_outcome.type == "plan_proposal", "outcome must have typed response plan_proposal");
    expect(plan_outcome.plan_proposal.has_value(), "plan proposal payload must be present");
    expect(!plan_outcome.plan_proposal->blocks.empty(), "plan must contain structured blocks");
    expect(plan_outcome.plan_proposal->status == "draft", "proposal must begin in draft state");

    const auto plan_id = plan_outcome.plan_proposal->id;

    // Verify intention not mutated before consent
    auto state_before = ledger.project_state();
    expect(state_before.intentions.front().precision == "date+period",
           "intention window must not change before plan application");

    // Apply plan with explicit consent
    const auto apply_outcome = assistant.apply_plan(plan_id, at("2026-09-21T18:06:00"));
    expect(apply_outcome.decision == "PLAN_APPLIED", "plan application must succeed");

    auto state_after = ledger.project_state();
    expect(state_after.plan_proposals.front().status == "applied", "plan status must update to applied");
    expect(state_after.intentions.front().allocated_plan_start.has_value(),
           "intention must have allocated plan slot");
    expect(state_after.intentions.front().precision == "date+period",
           "intention original declared precision must be preserved");

    // Test terminal state: cannot apply again
    const auto reapply = assistant.apply_plan(plan_id, at("2026-09-21T18:07:00"));
    expect(reapply.decision == "ALREADY_APPLIED", "cannot reapply applied terminal plan");
}

void plan_stale_detection_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    assistant.say("Amanhã de manhã quero escrever documentação.", at("2026-09-21T18:00:00"));
    const auto plan_outcome = assistant.say("Organiza minha manhã", at("2026-09-21T18:05:00"));
    expect(plan_outcome.decision == "PLAN_PROPOSED", "plan proposed");
    const auto plan_id = plan_outcome.plan_proposal->id;

    // Simulate intention completion / change before applying plan
    assistant.observe(at("2026-09-22T08:00:00"));
    assistant.reply("já resolvi", at("2026-09-22T08:01:00"));

    // Attempting to apply stale plan must return PLAN_STALE
    const auto stale_apply = assistant.apply_plan(plan_id, at("2026-09-22T08:05:00"));
    expect(stale_apply.decision == "PLAN_STALE", "applying stale plan must return PLAN_STALE");
}

void attention_candidate_factual_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    assistant.say("Amanhã de manhã quero trabalhar no artigo.", at("2026-09-21T18:00:00"));

    // Outside window (e.g. today at 18:00) -> nullopt
    auto att_outside = assistant.compute_attention_candidate(at("2026-09-21T18:05:00"));
    expect(!att_outside.has_value(), "must not claim attention when window is not active");

    // Inside window tomorrow morning (e.g. tomorrow at 09:00) -> active
    auto att_inside = assistant.compute_attention_candidate(at("2026-09-22T09:00:00"));
    expect(att_inside.has_value(), "must identify active attention candidate inside window");
    expect(att_inside->is_active_now == true, "candidate must be marked active now");
    expect(att_inside->subject == "trabalhar no artigo", "candidate subject must match");
}

void malformed_ledger_reject_test(const std::filesystem::path& path) {
    // Write a corrupted ledger file with truncated fields
    std::ofstream out(path);
    out << "LUME-LEDGER\t1\n";
    out << "1\t2026-09-21T18:00:00Z\tUNKNOWN_EVENT_TYPE\tuser\n"; // Unknown event type
    out.close();

    bool caught = false;
    try {
        lume::Ledger ledger{path};
        static_cast<void>(ledger.read_all());
    } catch (const std::exception&) {
        caught = true;
    }
    expect(caught, "malformed event type must be strictly rejected");
}

void sqlite_wal_persistence_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    ledger.append(lume::EventRecord{
        .recorded_at = at("2026-09-21T18:00:00"),
        .authority = "user",
        .epistemic_class = lume::EpistemicClass::user_declared,
        .payload = lume::EventExpressionRecorded{1, at("2026-09-21T18:00:00"), "teste sqlite wal"},
    });

    // Verify it is a valid SQLite 3 database
    std::ifstream in(path, std::ios::binary);
    char header[16];
    in.read(header, 16);
    expect(std::string_view(header, 16).starts_with("SQLite format 3"), "ledger must be created as SQLite format 3");

    const auto records = ledger.read_all();
    expect(records.size() == 1, "must retrieve 1 record from SQLite database");
    expect(records.front().sequence_number == 1, "first sequence number must be 1");
}

void epistemic_class_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    assistant.say("Amanhã de manhã quero estudar Rust.", at("2026-09-21T18:00:00"));
    const auto records = ledger.read_all();
    expect(records.size() >= 2, "must have recorded expression and derived intention");
    expect(records[0].epistemic_class == lume::EpistemicClass::user_declared, "expression is user_declared");
    expect(records[1].epistemic_class == lume::EpistemicClass::derived, "derived intention is derived");
}

void automation_creation_and_status_toggle_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    const auto created = assistant.create_automation("Organização Matinal", "08:30", "has_open_intentions",
                                                    "propose_daily_plan", "prepare_proposal",
                                                    at("2026-09-21T18:00:00"));
    expect(created.decision == "AUTOMATION_CREATED", "should create automation");
    expect(created.automation_proposal.has_value(), "should return created proposal");
    expect(created.automation_proposal->status == "active", "status should be active");

    const auto auto_id = created.automation_proposal->id;

    // Toggle to paused
    const auto toggled = assistant.toggle_automation(auto_id, "paused", at("2026-09-21T18:01:00"));
    expect(toggled.decision == "AUTOMATION_STATUS_CHANGED", "status change should succeed");

    auto state = ledger.project_state();
    expect(state.automation_proposals.front().status == "paused", "state must reflect paused");

    // Toggle back to active
    assistant.toggle_automation(auto_id, "active", at("2026-09-21T18:02:00"));
    state = ledger.project_state();
    expect(state.automation_proposals.front().status == "active", "state must reflect active");
}

void tick_evaluation_morning_plan_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    // User adds an intention
    assistant.say("Amanhã de manhã quero finalizar o relatório.", at("2026-09-21T18:00:00"));

    // Create daily routine at 08:30
    assistant.create_automation("Organização Matinal", "08:30", "has_open_intentions",
                               "propose_daily_plan", "prepare_proposal",
                               at("2026-09-21T18:01:00"));

    // Tick before 08:30 (e.g. 08:00) -> IDLE
    const auto tick_early = assistant.tick(at("2026-09-22T08:00:00"));
    expect(tick_early.decision == "TICK_IDLE", "must be idle before trigger time");
    expect(tick_early.emitted_notifications.empty(), "no notifications when idle");

    // Tick at 08:30 -> TRIGGERED
    const auto tick_trigger = assistant.tick(at("2026-09-22T08:30:00"));
    expect(tick_trigger.decision == "TICK_TRIGGERED", "must trigger at scheduled time");
    expect(!tick_trigger.emitted_notifications.empty(), "must emit notification");
    expect(tick_trigger.plan_proposal.has_value(), "must generate plan proposal");
    expect(tick_trigger.plan_proposal->status == "draft", "proposal must be in draft");

    const auto state = ledger.project_state();
    expect(state.notifications.size() == 1, "ledger must record emitted notification");
    expect(state.plan_proposals.size() == 1, "ledger must record generated plan proposal");
    expect(state.automation_proposals.front().last_triggered_at.has_value(), "must record last_triggered_at");
}

void tick_evaluation_no_duplicate_triggers_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    assistant.say("Amanhã de manhã quero revisar o código.", at("2026-09-21T18:00:00"));
    assistant.create_automation("Organização Matinal", "08:30", "has_open_intentions",
                               "propose_daily_plan", "prepare_proposal",
                               at("2026-09-21T18:01:00"));

    // First trigger at 08:30
    assistant.tick(at("2026-09-22T08:30:00"));

    // Second tick at 08:30 in same minute or same day
    const auto tick_second = assistant.tick(at("2026-09-22T08:30:30"));
    expect(tick_second.decision == "TICK_IDLE", "must not trigger twice on the same day");

    const auto state = ledger.project_state();
    expect(state.notifications.size() == 1, "notifications count must remain 1");
    expect(state.plan_proposals.size() == 1, "plan proposals count must remain 1");
}

void tick_eod_review_test(const std::filesystem::path& path) {
    lume::Ledger ledger{path};
    lume::Assistant assistant{ledger, lume::make_deterministic_language()};

    assistant.say("Amanhã de manhã quero escrever artigo.", at("2026-09-21T18:00:00"));
    assistant.create_automation("Fechamento do Dia", "18:00", "has_open_intentions",
                               "ask_eod_review", "suggest_only",
                               at("2026-09-21T18:01:00"));

    const auto tick_eod = assistant.tick(at("2026-09-22T18:00:00"));
    expect(tick_eod.decision == "TICK_TRIGGERED", "eod routine must trigger");
    expect(tick_eod.emitted_notifications.size() == 1, "one notification emitted");
    expect(tick_eod.emitted_notifications.front().action_type == "eod_prompt", "action_type must be eod_prompt");
}

}  // namespace

int main() {
    setenv("TZ", "UTC", 1);
    tzset();
    const auto base = std::filesystem::temp_directory_path() /
                      ("lume-tests-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(base);
    try {
        lifecycle_test(base / "lifecycle.state");
        authority_boundary_test(base / "authority.state");
        semantic_validation_test(base / "semantic-validation.state");
        empty_state_file_test(base / "empty.state");
        ambiguous_reply_test(base / "ambiguous-reply.state");
        unspecified_deferral_test(base / "unspecified-deferral.state");
        ledger_immutability_and_replay_test(base / "ledger-replay.state");
        legacy_migration_test(base / "legacy.state");
        sqlite_wal_persistence_test(base / "sqlite-wal.state");
        monotonic_sequence_test(base / "sequence.state");
        orchestration_plan_lifecycle_test(base / "orchestration.state");
        plan_stale_detection_test(base / "stale-plan.state");
        attention_candidate_factual_test(base / "attention.state");
        malformed_ledger_reject_test(base / "malformed.state");
        epistemic_class_test(base / "epistemic.state");
        automation_creation_and_status_toggle_test(base / "auto-toggle.state");
        tick_evaluation_morning_plan_test(base / "auto-tick-morning.state");
        tick_evaluation_no_duplicate_triggers_test(base / "auto-tick-dedup.state");
        tick_eod_review_test(base / "auto-tick-eod.state");
        std::filesystem::remove_all(base);
        std::cout << "all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(base);
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
