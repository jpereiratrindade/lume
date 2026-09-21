#pragma once

#include "lume/domain.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lume {

struct ContextProjection {
    std::string active_subject;
    std::vector<std::string> recent_open_subjects;
    std::vector<std::string> recent_facts;
    std::vector<Intention> open_intentions;
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
    std::string kind; // "intention", "plan_request", "unresolved_expression", etc.
    std::string subject;
    TemporalConstraint temporal;
    std::string precision;
    double confidence{};
    std::vector<std::string> ambiguities;
    std::string source;
};

struct PlanRequest {
    std::string utterance;
    std::string horizon;
    TimePoint reference_time{};
    std::vector<Intention> open_intentions;
};

struct PlanProposalCandidate {
    std::string horizon;
    std::string summary;
    std::vector<PlanBlock> blocks;
    std::vector<std::string> points_of_attention;
    double confidence{1.0};
    std::string source;
};

struct FormulationRequest {
    std::string decision;
    std::string subject;
    std::string reason;
};

struct FormulationResult {
    std::string text;
    std::string source;
};

class LanguageProvider {
public:
    virtual ~LanguageProvider() = default;
    [[nodiscard]] virtual InterpretationCandidate interpret(const InterpretationRequest& request) = 0;
    [[nodiscard]] virtual PlanProposalCandidate propose_plan(const PlanRequest& request) = 0;
    [[nodiscard]] virtual FormulationResult formulate(const FormulationRequest& request) = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

// Always available. It deliberately recognizes only a small, explicit grammar.
class DeterministicLanguage final : public LanguageProvider {
public:
    [[nodiscard]] InterpretationCandidate interpret(const InterpretationRequest& request) override;
    [[nodiscard]] PlanProposalCandidate propose_plan(const PlanRequest& request) override;
    [[nodiscard]] FormulationResult formulate(const FormulationRequest& request) override;
    [[nodiscard]] std::string name() const override;
};

std::unique_ptr<LanguageProvider> make_deterministic_language();
// Selects a configured local OpenAI-compatible server (Ollama or llama-server)
// when the optional HTTP integration is available, otherwise the deterministic provider.
std::unique_ptr<LanguageProvider> make_configured_language();
std::string fold_portuguese(std::string_view input);

}  // namespace lume
