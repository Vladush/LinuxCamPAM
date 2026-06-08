#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>

struct SensorState {
    int distance_cm;
    bool human_present;
};

class SensorParser {
public:
    virtual ~SensorParser() = default;

    SensorParser() = default;
    SensorParser(const SensorParser&) = delete;
    SensorParser& operator=(const SensorParser&) = delete;
    SensorParser(SensorParser&&) = delete;
    SensorParser& operator=(SensorParser&&) = delete;

    [[nodiscard]] virtual std::optional<SensorState> parse_payload(const uint8_t* buffer, size_t length) noexcept = 0;
};
