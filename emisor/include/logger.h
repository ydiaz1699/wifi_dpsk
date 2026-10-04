#pragma once
#include <Arduino.h>

#define LOG_DEBUG(fmt, ...) Serial.printf("[D] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  Serial.printf("[I] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Serial.printf("[W] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Serial.printf("[E] " fmt "\n", ##__VA_ARGS__)
