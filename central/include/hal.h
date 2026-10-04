#pragma once
#include <Arduino.h>

class Led {
    int _pin;
    bool _state;
public:
    explicit Led(int pin);
    void begin();
    void on();
    void off();
    void toggle();
    bool isOn() const;
};

class Buzzer {
    int _pin;
    bool _state;
    unsigned long _offTime;
    bool _timed;
    Led* _led;
public:
    explicit Buzzer(int pin);
    void begin();
    void on();
    void off();
    void timedOn(unsigned long ms);
    void setLed(Led* led);
    void loop();
    bool isOn() const;
};
