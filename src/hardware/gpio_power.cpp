#include "hardware/gpio_power.hpp"
#include <iostream>

// preprocessor conditionals
#if defined(__linux__) && __has_include(<gpiod.h>)
  #include <gpiod.h>
  #define HAS_LIBGPIOD 1
#else
  #define HAS_LIBGPIOD 0
#endif

GPIOPower::GPIOPower(int bcm_pin, bool active_high,
                     const std::string& chip_name,
                     const std::string& consumer)
    : pin_(bcm_pin), active_high_(active_high),
      chip_name_(chip_name), consumer_(consumer)
{
#if HAS_LIBGPIOD
    chip_ = gpiod_chip_open_by_name(chip_name_.c_str());
    if (!chip_) {
        std::cerr << "GPIO: cannot open " << chip_name_ << "\n";
        return;
    }
    line_ = gpiod_chip_get_line(chip_, pin_);
    if (!line_) {
        std::cerr << "GPIO: cannot get line for BCM " << pin_ << "\n";
        gpiod_chip_close(chip_);
        chip_ = nullptr;
    }
#else
    std::cerr << "GPIO: libgpiod unavailable (sim environment) — GPIO disabled\n";
    chip_ = nullptr;
    line_ = nullptr;
#endif
}

GPIOPower::~GPIOPower() {
#if HAS_LIBGPIOD
    if (requested_ && line_) gpiod_line_release(line_);
    if (chip_) gpiod_chip_close(chip_);
#endif
}

bool GPIOPower::enable() {
#if HAS_LIBGPIOD
    if (!line_) return false;
    if (requested_) { gpiod_line_release(line_); requested_ = false; }
    int val = active_high_ ? 1 : 0;
    if (gpiod_line_request_output(line_, consumer_.c_str(), val) != 0) {
        return false;
    }
    requested_ = true;
    return true;
#else
    return false;
#endif
}

bool GPIOPower::disable() {
#if HAS_LIBGPIOD
    if (!line_) return false;
    if (requested_) { gpiod_line_release(line_); requested_ = false; }
    int val = active_high_ ? 0 : 1;
    if (gpiod_line_request_output(line_, consumer_.c_str(), val) != 0) {
        return false;
    }
    requested_ = true;
    return true;
#else
    return false;
#endif
}