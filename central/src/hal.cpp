#include "hal.h"

Led::Led(int pin) : _pin(pin), _state(false) {}
void Led::begin() { pinMode(_pin, OUTPUT); off(); }
void Led::on() { digitalWrite(_pin, HIGH); _state = true; }
void Led::off() { digitalWrite(_pin, LOW); _state = false; }
void Led::toggle() { _state ? off() : on(); }
bool Led::isOn() const { return _state; }

Buzzer::Buzzer(int pin) : _pin(pin), _state(false), _offTime(0), _timed(false), _led(nullptr) {}
void Buzzer::begin() { pinMode(_pin, OUTPUT); off(); }
void Buzzer::setLed(Led* led) { _led = led; }
void Buzzer::on() {
    digitalWrite(_pin, HIGH);
    _state = true;
    _timed = false;
    if (_led) _led->on();
}
void Buzzer::off() {
    digitalWrite(_pin, LOW);
    _state = false;
    _timed = false;
    _offTime = 0;
    if (_led) _led->off();
}
void Buzzer::timedOn(unsigned long ms) {
    on();
    _offTime = millis() + ms;
    _timed = true;
}
void Buzzer::loop() {
    // Comparación signed de diferencia: funciona también al envolver millis().
    if (_state && _timed && static_cast<long>(millis() - _offTime) >= 0) off();
}
bool Buzzer::isOn() const { return _state; }
