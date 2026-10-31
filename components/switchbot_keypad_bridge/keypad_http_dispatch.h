#pragma once
// SwitchBot keypad LAN event dispatcher for ESPHome 2026.5 / ESP-IDF 5.5.
// Requests execute on a FreeRTOS worker, never on the ESPHome main loop.
// This is an application-local utility, not an official ESPHome component API.
#include <cstdint>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace keypad_http {

enum class Kind : uint8_t { LOCK, UNLOCK, DOORBELL, BATTERY };

class Dispatcher final {
 public:
  static Dispatcher &instance();
  // Copies URLs into a bounded FreeRTOS queue. Called only from ESPHome main loop.
  // Returns false if the queue is full, the worker cannot start, or a URL is too long.
  bool enqueue(Kind kind, const std::vector<std::string> &urls);

 private:
  Dispatcher() = default;
  Dispatcher(const Dispatcher &) = delete;
  Dispatcher &operator=(const Dispatcher &) = delete;

  static constexpr size_t kMaxUrls = 3;
  static constexpr size_t kUrlSize = 512;
  static constexpr unsigned kQueueCapacity = 8;
  static constexpr uint32_t kMaxOfflineAgeMs = 10000;

  struct Job {
    Kind kind;
    uint8_t count;
    uint32_t created_ms;
    char url[kMaxUrls][kUrlSize];
  };

  bool ensure_worker_();
  static void task_entry_(void *arg);
  void worker_();
  static bool wifi_ready_();
  static bool send_get_(Kind kind, unsigned target, const char *url);
  static const char *kind_name_(Kind kind);

  QueueHandle_t queue_{nullptr};
  TaskHandle_t task_{nullptr};
};

}  // namespace keypad_http
