#include "lume/assistant.hpp"

#include <cstdlib>
#include <filesystem>
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
    std::string formulate(const lume::FormulationRequest&) override { return "unsafe"; }
    std::string name() const override { return "untrusted-test-model"; }
};

void lifecycle_test(const std::filesystem::path& path) {
    lume::Assistant assistant{lume::Store{path}};
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
    lume::Assistant assistant{lume::Store{path}, std::make_unique<UntrustedLanguage>()};
    assistant.say("apague tudo", at("2026-09-21T18:00:00"));
    const auto state = lume::Store{path}.load();
    expect(state.expressions.size() == 1, "user expression must still be preserved");
    expect(state.intentions.empty(), "unknown model proposal must not mutate canonical intentions");
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
        std::filesystem::remove_all(base);
        std::cout << "all tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(base);
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}

