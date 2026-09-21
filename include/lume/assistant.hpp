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

    [[nodiscard]] std::string explain_last() const;
    [[nodiscard]] std::string inspect() const;
    [[nodiscard]] std::string language_name() const;
    [[nodiscard]] const Ledger& ledger() const noexcept;

private:
    Ledger ledger_;
    std::unique_ptr<LanguageProvider> language_;
};

}  // namespace lume
