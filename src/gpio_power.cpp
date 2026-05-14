#include "gpio_power.hpp"
#include <gpiod.h>
#include <iostream>

GPIOPower::GPIOPower(int bcm_pin, bool active_high,
                     const std::string& chip_name,
                     const std::string& consumer)
    : pin_(bcm_pin), active_high_(active_high),
      chip_name_(chip_name), consumer_(consumer)
{
    chip_ = gpiod_chip_open_by_name(chip_name_.c_str());
    if (!chip_) {
        std::cerr << "GPIO: " << chip_name_
                  << " acilamadi. libgpiod-dev kurulu mu? "
                  << "Kullanici gpio grubunda mi?\n";
        return;
    }
    line_ = gpiod_chip_get_line(chip_, pin_);
    if (!line_) {
        std::cerr << "GPIO: BCM " << pin_ << " hatti alinamadi\n";
        gpiod_chip_close(chip_);
        chip_ = nullptr;
    }
}

GPIOPower::~GPIOPower() {
    if (requested_ && line_) gpiod_line_release(line_);
    if (chip_) gpiod_chip_close(chip_);
}

bool GPIOPower::enable() {
    if (!line_) return false;
    if (requested_) { gpiod_line_release(line_); requested_ = false; }
    int val = active_high_ ? 1 : 0;
    if (gpiod_line_request_output(line_, consumer_.c_str(), val) != 0) {
        std::cerr << "GPIO: BCM " << pin_ << " output request basarisiz "
                  << "(zaten baska bir proc tarafindan tutuluyor olabilir)\n";
        return false;
    }
    requested_ = true;
    return true;
}

bool GPIOPower::disable() {
    if (!line_) return false;
    if (requested_) { gpiod_line_release(line_); requested_ = false; }
    int val = active_high_ ? 0 : 1;
    if (gpiod_line_request_output(line_, consumer_.c_str(), val) != 0) return false;
    requested_ = true;
    return true;
}