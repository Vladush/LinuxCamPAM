#pragma once

#include "SensorParser.hpp"
#include <cstring>
#include <array>

class Ite8353Parser final : public SensorParser {
private:
    static constexpr size_t RESERVED_1_SIZE = 6; // Padding bytes defined by the hardware specification
    static constexpr size_t RESERVED_2_SIZE = 3; // Additional padding bytes defined by the hardware spec
    static constexpr size_t PACKET_SIZE = 12; // Total expected size of the binary payload
    static constexpr uint8_t MAGIC_BYTE = 0x02; // Identifier byte to ensure a valid packet is read

#pragma pack(push, 1)
    struct PayloadFormat {
        uint8_t magic;
        std::array<uint8_t, RESERVED_1_SIZE> reserved1;
        uint8_t confidence; // Note: Interpreted empirically as a confidence score or timeout (0-100), not a physical distance.
        std::array<uint8_t, RESERVED_2_SIZE> reserved2;
        uint8_t presence;
    };
#pragma pack(pop)
    static_assert(sizeof(PayloadFormat) == PACKET_SIZE, "Payload layout mismatch");

public:
    [[nodiscard]] std::optional<SensorState> parse_payload(const uint8_t* buffer, size_t length) noexcept override {
        if (buffer == nullptr || length < sizeof(PayloadFormat)) return std::nullopt; 
        
        PayloadFormat payload{};
        std::memcpy(&payload, buffer, sizeof(PayloadFormat));
        
        if (payload.magic == MAGIC_BYTE) {
            return SensorState{payload.confidence, payload.presence == 1};
        }
        return std::nullopt;
    }
};
