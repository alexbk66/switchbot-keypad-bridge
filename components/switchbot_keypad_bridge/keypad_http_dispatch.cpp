#include "keypad_http_dispatch.h"

#include <cstdio>
#include <cstring>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esphome/core/log.h"

namespace keypad_http {
namespace {
constexpr const char *TAG = "keypad_http";
uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
}  // namespace

Dispatcher &Dispatcher::instance() {
  static Dispatcher dispatcher;
  return dispatcher;
}

const char *Dispatcher::kind_name_(Kind kind) {
  switch (kind) {
    case Kind::LOCK: return "lock";
    case Kind::UNLOCK: return "unlock";
    case Kind::DOORBELL: return "doorbell";
    case Kind::BATTERY: return "battery";
  }
  return "unknown";
}

bool Dispatcher::ensure_worker_() {
  if (this->queue_ == nullptr) {
    this->queue_ = xQueueCreate(kQueueCapacity, sizeof(Job));
    if (this->queue_ == nullptr) {
      ESP_LOGE(TAG, "Unable to allocate HTTP dispatch queue");
      return false;
    }
  }
  if (this->task_ == nullptr) {
    // Dedicated worker: blocking TCP/DNS never stalls the ESPHome/BLE loop.
    if (xTaskCreate(&Dispatcher::task_entry_, "keypad_http", 8192, this,
                    tskIDLE_PRIORITY + 1, &this->task_) != pdPASS) {
      ESP_LOGE(TAG, "Unable to start HTTP dispatch worker");
      this->task_ = nullptr;
      return false;
    }
    ESP_LOGI(TAG, "HTTP dispatch worker started");
  }
  return true;
}

bool Dispatcher::enqueue(Kind kind, const std::vector<std::string> &urls) {
  if (urls.empty() || urls.size() > kMaxUrls) {
    ESP_LOGE(TAG, "%s rejected: invalid URL count %u", kind_name_(kind),
             static_cast<unsigned>(urls.size()));
    return false;
  }
  Job job{};
  job.kind = kind;
  job.count = static_cast<uint8_t>(urls.size());
  job.created_ms = uptime_ms();
  for (size_t i = 0; i < urls.size(); ++i) {
    // Keep URL and secrets out of diagnostic output.
    if (urls[i].size() >= kUrlSize || urls[i].compare(0, 7, "http://") != 0) {
      ESP_LOGE(TAG, "%s rejected: target %u has long/non-HTTP URL", kind_name_(kind),
               static_cast<unsigned>(i));
      return false;
    }
    std::memcpy(job.url[i], urls[i].c_str(), urls[i].size() + 1);
  }
  if (!this->ensure_worker_()) return false;
  if (xQueueSend(this->queue_, &job, 0) != pdPASS) {
    ESP_LOGE(TAG, "%s dropped: HTTP dispatch queue full", kind_name_(kind));
    return false;
  }
  ESP_LOGD(TAG, "%s queued (%u target(s))", kind_name_(kind), job.count);
  return true;
}

bool Dispatcher::wifi_ready_() {
  // ESPHome's ESP-IDF WiFi component creates the default station netif.
  auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == nullptr) return false;
  esp_netif_ip_info_t ip{};
  if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
  wifi_ap_record_t ap{};
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool Dispatcher::send_get_(Kind kind, unsigned target, const char *url) {
  esp_http_client_config_t config{};
  config.url = url;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 3000;
  config.disable_auto_redirect = true;
  config.keep_alive_enable = false;
  auto client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(TAG, "%s target %u: HTTP client init failed", kind_name_(kind), target);
    return false;
  }
  const esp_err_t result = esp_http_client_perform(client);
  const int status = result == ESP_OK ? esp_http_client_get_status_code(client) : -1;
  esp_http_client_cleanup(client);
  const bool ok = result == ESP_OK && status >= 200 && status < 300;
  if (ok) {
    ESP_LOGD(TAG, "%s target %u: HTTP %d", kind_name_(kind), target, status);
  } else {
    ESP_LOGW(TAG, "%s target %u: HTTP failed (%s, status %d)",
             kind_name_(kind), target, esp_err_to_name(result), status);
  }
  return ok;
}

void Dispatcher::task_entry_(void *arg) {
  static_cast<Dispatcher *>(arg)->worker_();
}

void Dispatcher::worker_() {
  Job job{};
  for (;;) {
    if (xQueueReceive(this->queue_, &job, portMAX_DELAY) != pdTRUE) continue;
    // Fail closed: an unlock that was not dispatched promptly must not occur
    // minutes later after the user has already entered another way.
    while (!wifi_ready_() && uint32_t(uptime_ms() - job.created_ms) < kMaxOfflineAgeMs) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!wifi_ready_() || uint32_t(uptime_ms() - job.created_ms) >= kMaxOfflineAgeMs) {
      ESP_LOGW(TAG, "%s dropped: Wi-Fi unavailable or event stale", kind_name_(job.kind));
      continue;
    }
    for (unsigned i = 0; i < job.count; ++i) {
      // First target is the physical lock for lock/unlock.
      // Never automatically retry physical-lock GETs: a timeout may occur
      // *after* the lock already acted. Repeating could be unsafe.
      const bool success = send_get_(job.kind, i, job.url[i]);
      if (!success && (job.kind == Kind::BATTERY || job.kind == Kind::DOORBELL || i > 0)) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (wifi_ready_()) send_get_(job.kind, i, job.url[i]);
      }
    }
  }
}

}  // namespace keypad_http
