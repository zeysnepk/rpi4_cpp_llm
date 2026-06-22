#pragma once
#include <string>

struct gpiod_chip;
struct gpiod_line;

// Controls a single GPIO pin to power on/off the sensor rail.
class GPIOPower {
public:
    // consructor
    GPIOPower(int bcm_pin,
              bool active_high = true,
              const std::string& chip_name = "gpiochip0",
              const std::string& consumer  = "rpi4_dashboard");
    ~GPIOPower(); // destructor

    bool enable();    // assert the power-enable line
    bool disable();   // deassert the power-enable line
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