#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lume {

struct ContextProjection {
    std::string active_subject;
    std::vector<std::string> recent_open_subjects;
};

struct InterpretationRequest {
    std::string utterance;
    ContextProjection context;
};

struct TemporalConstraint {
    std::string date_reference;
    std::string period;
};

struct InterpretationCandidate {
    std::string kind;
    std::string subject;
    TemporalConstraint temporal;
    std::string precision;
    double confidence{};
    std::vector<std::string> ambiguities;
};

struct FormulationRequest {
    std::string decision;
    std::string subject;
    std::string reason;
};

class LanguageProvider {
public:
    virtual ~LanguageProvider() = default;
    [[nodiscard]] virtual InterpretationCandidate interpret(const InterpretationRequest& request) = 0;
    [[nodiscard]] virtual std::string formulate(const FormulationRequest& request) = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

// Always available. It deliberately recognizes only a small, explicit grammar.
class DeterministicLanguage final : public LanguageProvider {
public:
    [[nodiscard]] InterpretationCandidate interpret(const InterpretationRequest& request) override;
    [[nodiscard]] std::string formulate(const FormulationRequest& request) override;
    [[nodiscard]] std::string name() const override;
};

std::unique_ptr<LanguageProvider> make_deterministic_language();
std::string fold_portuguese(std::string_view input);

}  // namespace lume

