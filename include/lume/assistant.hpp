#pragma once

#include "lume/domain.hpp"
#include "lume/language.hpp"
#include "lume/store.hpp"

#include <memory>
#include <string_view>

namespace lume {

class Assistant {
public:
    explicit Assistant(Store store);
    Assistant(Store store, std::unique_ptr<LanguageProvider> language);

    Outcome say(std::string_view expression, TimePoint now);
    Outcome observe(TimePoint now);
    Outcome reply(std::string_view response, TimePoint now);
    [[nodiscard]] std::string explain_last() const;
    [[nodiscard]] std::string inspect() const;

private:
    Store store_;
    std::unique_ptr<LanguageProvider> language_;
};

}  // namespace lume
