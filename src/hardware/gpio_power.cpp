#include "hardware/gpio_power.hpp"
#include <iostream>

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
    chip_ = gpiod_chip_open(("/dev/" + chip_name_).c_str());
    if (!chip_) {
        std::cerr << "GPIO: cannot open " << chip_name_ << "\n";
        return;
    }

    auto* settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings,
        active_high_ ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE);

    auto* line_cfg = gpiod_line_config_new();
    unsigned int offset = static_cast<unsigned int>(pin_);
    gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);

    auto* req_cfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(req_cfg, consumer_.c_str());

    request_ = gpiod_chip_request_lines(chip_, req_cfg, line_cfg);

    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);

    if (!request_) {
        std::cerr << "GPIO: cannot request line BCM " << pin_ << "\n";
        gpiod_chip_close(chip_);
        chip_ = nullptr;
    }
#else
    std::cerr << "GPIO: libgpiod unavailable (sim environment) — GPIO disabled\n";
    chip_ = nullptr;
    request_ = nullptr;
#endif
}

GPIOPower::~GPIOPower() {
#if HAS_LIBGPIOD
    if (request_) gpiod_line_request_release(request_);
    if (chip_) gpiod_chip_close(chip_);
#endif
}

bool GPIOPower::enable() {
#if HAS_LIBGPIOD
    if (!request_) return false;
    unsigned int offset = static_cast<unsigned int>(pin_);
    auto val = active_high_ ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    return gpiod_line_request_set_value(request_, offset, val) == 0;
#else
    return false;
#endif
}

bool GPIOPower::disable() {
#if HAS_LIBGPIOD
    if (!request_) return false;
    unsigned int offset = static_cast<unsigned int>(pin_);
    auto val = active_high_ ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE;
    return gpiod_line_request_set_value(request_, offset, val) == 0;
#else
    return false;
#endif
}
