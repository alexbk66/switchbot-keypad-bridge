#include <cassert>
#include <cstdint>
#include "components/switchbot_keypad_bridge/keypad_advert.h"
using namespace esphome::switchbot_keypad_bridge;

int main() {
  const uint8_t vision_pro[15] = {
      0x69, 0x09, 0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(0x80 | 82),
      0x02, 0, 0, 0, 0};
  auto s = parse_keypad_vision_status(vision_pro, sizeof(vision_pro));
  assert(s.battery == 82 && s.charging == 1 && s.tamper_alarm == 1);
  assert(s.lockout_alarm == 0 && s.duress_alarm == 0);
  uint8_t flags[15] = {};
  for (unsigned i = 0; i < sizeof(flags); i++) flags[i] = vision_pro[i];
  flags[10] = 0x05; // lockout + duress, tamper clear
  s = parse_keypad_vision_status(flags, sizeof(flags));
  assert(s.tamper_alarm == 0 && s.lockout_alarm == 1 && s.duress_alarm == 1);
  s = parse_keypad_vision_status(vision_pro, 10);
  assert(s.battery == -1 && s.tamper_alarm == -1);
  s = parse_keypad_vision_status(vision_pro, 11);
  assert(s.battery == 82 && s.tamper_alarm == -1);
  flags[0] = 0;
  s = parse_keypad_vision_status(flags, sizeof(flags));
  assert(s.battery == -1);
}
