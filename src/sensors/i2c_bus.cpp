#include "i2c_bus.hpp"
#include <iostream>

#if defined(__linux__)
  #include <fcntl.h>
  #include <unistd.h>
  #include <sys/ioctl.h>
  #include <linux/i2c-dev.h>
  #define HAS_I2C 1
#else
  #define HAS_I2C 0
#endif

I2CBus::I2CBus(const std::string& device) {
#if HAS_I2C
    fd_ = open(device.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::cerr << "I2C: " << device << " acilamadi\n";
    }
#else
    (void)device;
    std::cerr << "I2C: Linux disinda (sim ortami), donanim I2C devre disi\n";
    fd_ = -1;
#endif
}

I2CBus::~I2CBus() {
#if HAS_I2C
    if (fd_ >= 0) close(fd_);
#endif
}

bool I2CBus::select_addr(uint8_t addr) {
#if HAS_I2C
    return ioctl(fd_, I2C_SLAVE, addr) >= 0;
#else
    (void)addr;
    return false;
#endif
}

bool I2CBus::write_byte(uint8_t addr, uint8_t reg, uint8_t value) {
#if HAS_I2C
    std::lock_guard<std::mutex> lk(mtx_);
    if (!select_addr(addr)) return false;
    uint8_t buf[2] = { reg, value };
    return ::write(fd_, buf, 2) == 2;
#else
    (void)addr; (void)reg; (void)value;
    return false;
#endif
}

bool I2CBus::read_byte(uint8_t addr, uint8_t reg, uint8_t& value) {
    return read_bytes(addr, reg, &value, 1);
}

bool I2CBus::read_bytes(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
#if HAS_I2C
    std::lock_guard<std::mutex> lk(mtx_);
    if (!select_addr(addr)) return false;
    if (::write(fd_, &reg, 1) != 1) return false;
    return ::read(fd_, buf, len) == (ssize_t)len;
#else
    (void)addr; (void)reg; (void)buf; (void)len;
    return false;
#endif
}