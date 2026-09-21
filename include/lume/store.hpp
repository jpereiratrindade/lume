#pragma once

#include "lume/domain.hpp"

#include <filesystem>

namespace lume {

class Store {
public:
    explicit Store(std::filesystem::path path);

    [[nodiscard]] State load() const;
    void save(const State& state) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    std::filesystem::path path_;
};

}  // namespace lume

