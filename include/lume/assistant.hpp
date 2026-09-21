#pragma once

#include "lume/domain.hpp"
#include "lume/language.hpp"
#include "lume/ledger.hpp"
#include "lume/store.hpp"

#include <memory>
#include <string_view>

namespace lume {

class Assistant {
public:
    explicit Assistant(Ledger ledger);
    Assistant(Ledger ledger, std::unique_ptr<LanguageProvider> language);
    explicit Assistant(Store store);
    Assistant(Store store, std::unique_ptr<LanguageProvider> language);

    Outcome say(std::string_view expression, TimePoint now);
    Outcome observe(TimePoint now);
    Outcome reply(std::string_view response, TimePoint now);
    Outcome plan(std::string_view horizon_or_request, TimePoint now);
    Outcome apply_plan(std::uint64_t plan_id, TimePoint now);
    Outcome discard_plan(std::uint64_t plan_id, TimePoint now);

    // Automation and Routine Engine
    Outcome create_automation(std::string_view title,
                              std::string_view trigger_when,
                              std::string_view condition_if,
                              std::string_view action_then,
                              std::string_view authority,
                              TimePoint now);
    Outcome toggle_automation(std::uint64_t automation_id, std::string_view new_status, TimePoint now);
    Outcome trigger_automation(std::uint64_t automation_id, TimePoint now);
    Outcome tick(TimePoint now);

    [[nodiscard]] std::optional<AttentionCandidate> compute_attention_candidate(TimePoint now) const;
    [[nodiscard]] std::string explain_last() const;
    [[nodiscard]] std::string inspect() const;
    [[nodiscard]] std::string inspect(TimePoint now) const;
    [[nodiscard]] std::string language_name() const;
    [[nodiscard]] const Ledger& ledger() const noexcept;

private:
    Ledger ledger_;
    std::unique_ptr<LanguageProvider> language_;
};

}  // namespace lume
