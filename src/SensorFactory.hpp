#pragma once

#include <memory>
#include <string_view>
#include <unordered_map>
#include <functional>
#include "SensorParser.hpp"
#include "parsers/Ite8353Parser.hpp"

class ISensorFactory {
public:
    virtual ~ISensorFactory() = default;
    ISensorFactory() = default;
    ISensorFactory(const ISensorFactory&) = delete;
    ISensorFactory& operator=(const ISensorFactory&) = delete;
    ISensorFactory(ISensorFactory&&) = delete;
    ISensorFactory& operator=(ISensorFactory&&) = delete;
    [[nodiscard]] virtual std::unique_ptr<SensorParser> create(std::string_view hardware_id) const = 0;
};

class SensorFactory : public ISensorFactory {
    using CreatorFunc = std::function<std::unique_ptr<SensorParser>()>;
    std::unordered_map<std::string_view, CreatorFunc> registry_;

public:
    SensorFactory() {
        registry_["ITE8353"] = []() { return std::make_unique<Ite8353Parser>(); };
    }

    [[nodiscard]] std::unique_ptr<SensorParser> create(std::string_view hardware_id) const override {
        if (auto it = registry_.find(hardware_id); it != registry_.end()) {
            return it->second();
        }
        return nullptr;
    }
};
