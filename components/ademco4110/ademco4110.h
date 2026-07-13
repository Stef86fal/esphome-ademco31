#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include <map>
#include <string>
#include <driver/gpio.h>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace ademco4110 {

static const gpio_num_t TX_GPIO    = GPIO_NUM_22;
static const int        RMT_CHAN   = 0;
static const uint8_t    RMT_DIV   = 80;
static const uint16_t   BIT_TICKS = 417;
static const uint64_t   SYNC_MIN_US = 171000;

enum ParseState { WAIT_HEADER, WAIT_SEP, READ_DATA };
enum SystemState {
  STATE_UNKNOWN, STATE_DISARMED, STATE_ARMED_TOTAL,
  STATE_ARMED_PARTIAL, STATE_ALARM,
};

class Ademco4110Component : public Component, public uart::UARTDevice {
 public:
  void set_sync_pin(InternalGPIOPin *p) { sync_pin_ = p; }

  void set_armed_sensor(binary_sensor::BinarySensor *s)         { armed_sensor_ = s; }
  void set_armed_total_sensor(binary_sensor::BinarySensor *s)   { armed_total_sensor_ = s; }
  void set_armed_partial_sensor(binary_sensor::BinarySensor *s) { armed_partial_sensor_ = s; }
  void set_alarm_sensor(binary_sensor::BinarySensor *s)         { alarm_sensor_ = s; }
  void set_chime_sensor(binary_sensor::BinarySensor *s)         { chime_sensor_ = s; }
  void set_ready_sensor(binary_sensor::BinarySensor *s)         { ready_sensor_ = s; }
  void set_ac_power_sensor(binary_sensor::BinarySensor *s)      { ac_power_sensor_ = s; }
  void set_battery_low_sensor(binary_sensor::BinarySensor *s)   { battery_low_sensor_ = s; }
  void set_bypass_sensor(binary_sensor::BinarySensor *s)        { bypass_sensor_ = s; }
  void set_status_sensor(text_sensor::TextSensor *s)            { status_sensor_ = s; }
  void set_diagnostic_mode(bool v)                              { diagnostic_mode_ = v; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void send_keys(const char *keys);
  void scan_zones();
  void set_zones_sensor(text_sensor::TextSensor *s) { zones_sensor_ = s; }
  void set_alarm_zone_sensor(text_sensor::TextSensor *s) { alarm_zone_sensor_ = s; }
  void set_disarm_code(const std::string &c) { disarm_code_ = c; }

 protected:
  InternalGPIOPin *sync_pin_{nullptr};

  binary_sensor::BinarySensor *armed_sensor_{nullptr};
  binary_sensor::BinarySensor *armed_total_sensor_{nullptr};
  binary_sensor::BinarySensor *armed_partial_sensor_{nullptr};
  binary_sensor::BinarySensor *alarm_sensor_{nullptr};
  binary_sensor::BinarySensor *chime_sensor_{nullptr};
  binary_sensor::BinarySensor *ready_sensor_{nullptr};
  binary_sensor::BinarySensor *ac_power_sensor_{nullptr};
  int8_t   ac_last_published_{-1};  // -1=mai pubblicato, 0=assente, 1=presente
  uint32_t last_noac_ms_{0};        // ultimo frame NO-AC visto (isteresi)
  binary_sensor::BinarySensor *battery_low_sensor_{nullptr};
  binary_sensor::BinarySensor *bypass_sensor_{nullptr};
  text_sensor::TextSensor     *status_sensor_{nullptr};
  text_sensor::TextSensor     *zones_sensor_{nullptr};
  text_sensor::TextSensor     *alarm_zone_sensor_{nullptr};

  // Scansione zone aperte (modalita' attivata da tasto *)
  bool        scanning_zones_{false};
  uint32_t    scan_start_ms_{0};
  uint32_t    scan_last_zone_ms_{0};
  uint16_t    scan_zones_mask_{0};   // bit 0 = zona 1, bit 7 = zona 8
  uint8_t     pending_zone_sep_{0};  // separatore pre-0x0C catturato durante lo scan
  static constexpr const char* ZONE_NAMES[9] = {
    "",                    // indice 0 (non usato)
    "ZONE 1",      // zona 1
    "ZONE 2",      // zona 2
    "ZONE 3",      // zona 3
    "ZONE 4",      // zona 4
    "ZONE 5",      // zona 5
    "ZONE 6",      // zona 6
    "ZONE 7",      // zona 7
    "ZONE 8"       // zona 8
  };
  std::string disarm_code_;

  // Votazione a maggioranza per la zona d'allarme.
  // Durante l'allarme il separatore pre-0x0C può ciclare e includere zone
  // spurie (separatori di servizio del display d'allarme). Raccogliamo i
  // conteggi per ~4s e pubblichiamo la zona più frequente, robusta a glitch.
  bool        alarm_voting_{false};        // finestra di voto attiva
  uint32_t    alarm_vote_start_ms_{0};     // inizio finestra
  uint8_t     alarm_zone_votes_[9]{};      // conteggi per zona 1..8
  uint8_t     alarm_first_zone_{0};        // prima zona vista (tie-break)
  static const uint32_t ALARM_VOTE_MS = 4000;  // durata finestra voto

  SystemState system_state_{STATE_UNKNOWN};
  uint32_t    last_msg_ms_{0};
  uint32_t    last_parse_ms_{0};

  ParseState  parse_state_{WAIT_HEADER};
  uint8_t     zero_count_{0};
  uint8_t     sep_byte_{0};
  uint8_t     frame_buf_[4]{};
  uint8_t     buf_pos_{0};
  uint8_t     pending_frame_[4]{};
  bool        pending_frame_valid_{false};
  uint8_t     prev_frame_[4]{};
  uint8_t     prev_sep_{0};
  bool        prev_frame_valid_{false};
  bool        partial_seen_{false};
  uint8_t     frame04_count_{0};

  uint8_t     keys_[10];
  uint8_t     num_keys_{0};
  uint8_t     key_idx_{0};
  bool        key_sending_{false};

  bool        diagnostic_mode_{false};

  void rmt_tx_init();
  void rmt_tx_send_key(uint8_t key);
  static void sync_task(void *arg);
  void process_status(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3);
  void process_armed(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3);
  void finish_zone_scan();
  void alarm_vote_separator(uint8_t sep);  // accumula voto zona durante allarme
  void finish_alarm_vote();                // pubblica zona vincente
  void set_system_state(SystemState s);
  void publish_all();
  const char *state_str(SystemState s);
  uint8_t char_to_key(char c);
};

}  // namespace ademco4110
}  // namespace esphome

#include "esphome/core/automation.h"
namespace esphome {
namespace ademco4110 {
template<typename... Ts>
class SendKeysAction : public Action<Ts...>, public Parented<Ademco4110Component> {
 public:
  TEMPLATABLE_VALUE(std::string, keys)
  void play(Ts... x) override {
    this->parent_->send_keys(this->keys_.value(x...).c_str());
  }
};
template<typename... Ts>
class ScanZonesAction : public Action<Ts...>, public Parented<Ademco4110Component> {
 public:
  void play(Ts... x) override {
    this->parent_->scan_zones();
  }
};
}  // namespace ademco4110
}  // namespace esphome
