#pragma once
#include <string>

struct gpiod_chip;
struct gpiod_line;

// Tek bir GPIO pinini kalici olarak HIGH/LOW tutar (sensor power-enable icin)
class GPIOPower {
public:
    GPIOPower(int bcm_pin,
              bool active_high = true,
              const std::string& chip_name = "gpiochip0",
              const std::string& consumer  = "rpi4_dashboard");
    ~GPIOPower();

    bool enable();    // sensorlere guc ver
    bool disable();   // sensorleri kapat
    bool is_open() const { return chip_ != nullptr && line_ != nullptr; }
    int  pin() const { return pin_; }

private:
    gpiod_chip* chip_ = nullptr;
    gpiod_line* line_ = nullptr;
    int pin_;
    [[maybe_unused]] bool active_high_;
    [[maybe_unused]] bool requested_ = false;
    std::string chip_name_;
    std::string consumer_;
};