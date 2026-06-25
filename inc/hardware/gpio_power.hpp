#pragma once
#include <string>

struct gpiod_chip;
struct gpiod_line_request;

class GPIOPower {
public:
    GPIOPower(int bcm_pin,
              bool active_high = true,
              const std::string& chip_name = "gpiochip0",
              const std::string& consumer  = "rpi4_dashboard");
    ~GPIOPower();

    bool enable();
    bool disable();
    bool is_open() const { return chip_ != nullptr && request_ != nullptr; }
    int  pin() const { return pin_; }

private:
    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    int pin_;
    bool active_high_;
    std::string chip_name_;
    std::string consumer_;
};