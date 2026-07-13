#include "ademco4110.h"
#include "esphome/core/log.h"
#include <esp_timer.h>
#include <esp_rom_gpio.h>
#include <driver/gpio.h>
#include <string.h>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include <driver/rmt.h>
#pragma GCC diagnostic pop

namespace esphome {
namespace ademco4110 {

static const char *TAG = "ademco4120";

// ─────────────────────────────────────────────────────────────
//  Mappatura bit — documentazione gregrenda non-addressable
//  Il frame di stato valido e' quello che precede 0x0C
//  (separatore variabile: 0xDC, 0xD4 ecc.)
//
//  Byte 2 (B2):
//    bit 4 0x10 = READY LED (verde) -> pronto
//    bit 5 0x20 = FIRE
//    bit 6 0x40 = BAT
//    bit 7 0x80 = STAY / red ARMED led
//
//  Byte 3 (B3):
//    bit 1 0x02 = ALARM
//    bit 2 0x04 = AWAY / red ARMED led
//    bit 3 0x08 = AC (disable NO AC)
//    bit 4 0x10 = BYPASS
//    bit 5 0x20 = CHIME
//    bit 6 0x40 = disable NOT READY
//    bit 7 0x80 = INSTANT
// ─────────────────────────────────────────────────────────────

void Ademco4110Component::rmt_tx_init() {
  rmt_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.rmt_mode                    = RMT_MODE_TX;
  cfg.channel                     = (rmt_channel_t)RMT_CHAN;
  cfg.gpio_num                    = TX_GPIO;
  cfg.clk_div                     = RMT_DIV;
  cfg.mem_block_num               = 1;
  cfg.tx_config.loop_en           = false;
  cfg.tx_config.carrier_en        = false;
  cfg.tx_config.idle_output_en    = true;
  cfg.tx_config.idle_level        = RMT_IDLE_LEVEL_HIGH;
  ESP_ERROR_CHECK(rmt_config(&cfg));
  ESP_ERROR_CHECK(rmt_driver_install((rmt_channel_t)RMT_CHAN, 0, 0));
  ESP_LOGI(TAG, "RMT TX inizializzato su GPIO%d", (int)TX_GPIO);
}

void Ademco4110Component::rmt_tx_send_key(uint8_t key) {
  rmt_item32_t items[16];
  memset(items, 0, sizeof(items));
  int idx = 0;
  auto encode_byte = [&](uint8_t val) {
    uint8_t parity = 0;
    for (int i = 0; i < 5; i++) parity ^= (val >> i) & 1;
    uint16_t frame = 0;
    for (int i = 0; i < 5; i++) frame |= ((val >> i) & 1) << (1 + i);
    frame |= (uint16_t)parity << 6;
    frame |= (1 << 7);
    frame |= (1 << 8);
    for (int i = 0; i < 8; i += 2) {
      items[idx].level0    = (frame >> i)     & 1;
      items[idx].duration0 = BIT_TICKS;
      items[idx].level1    = (frame >> (i+1)) & 1;
      items[idx].duration1 = BIT_TICKS;
      idx++;
    }
    items[idx].level0    = (frame >> 8) & 1;
    items[idx].duration0 = BIT_TICKS;
    items[idx].level1    = 1;
    items[idx].duration1 = 1;
    idx++;
  };
  encode_byte(key); encode_byte(key); encode_byte(key);
  items[idx].val = 0;
  ESP_ERROR_CHECK(rmt_write_items((rmt_channel_t)RMT_CHAN, items, idx + 1, true));
}

void Ademco4110Component::setup() {
  rmt_tx_init();
  if (sync_pin_) {
    sync_pin_->setup();
    sync_pin_->pin_mode(gpio::FLAG_INPUT);
    xTaskCreatePinnedToCore(sync_task, "sync_task", 2048, this, 24, nullptr, 0);
  }
  system_state_ = STATE_UNKNOWN;
  publish_all();
}

void Ademco4110Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Ademco 4120 v15g (voto zona allarme):");
  ESP_LOGCONFIG(TAG, "  TX GPIO: %d", (int)TX_GPIO);
  ESP_LOGCONFIG(TAG, "  sync_pin: %s", sync_pin_ ? "OK" : "non configurato");
  ESP_LOGCONFIG(TAG, "  diagnostic_mode: %s", diagnostic_mode_ ? "ON" : "off");
}

void Ademco4110Component::sync_task(void *arg) {
  auto *self = static_cast<Ademco4110Component *>(arg);
  gpio_num_t pin = (gpio_num_t)self->sync_pin_->get_pin();
  bool last = gpio_get_level(pin);
  uint64_t last_us = esp_timer_get_time();
  while (true) {
    bool cur = gpio_get_level(pin);
    if (cur != last) {
      uint64_t now_us = esp_timer_get_time();
      uint64_t diff = now_us - last_us;
      last_us = now_us; last = cur;
      if (diff > SYNC_MIN_US) {
        if (!self->key_sending_ && self->num_keys_ > 0 &&
            self->key_idx_ < self->num_keys_) {
          self->key_sending_ = true;
          uint8_t key = self->keys_[self->key_idx_++];
          vTaskDelay(pdMS_TO_TICKS(4));
          self->rmt_tx_send_key(key);
          if (self->key_idx_ >= self->num_keys_) {
            self->num_keys_ = 0; self->key_idx_ = 0;
          }
          self->key_sending_ = false;
        }
      }
    }
    vTaskDelay(1);
  }
}

void Ademco4110Component::loop() {
  uint32_t now = millis();

  if (parse_state_ != WAIT_HEADER && last_parse_ms_ && (now - last_parse_ms_) > 2000) {
    ESP_LOGW(TAG, "Parser reset");
    parse_state_ = WAIT_HEADER;
    zero_count_ = 0;
    buf_pos_ = 0;
    while (available()) { uint8_t tmp; read_byte(&tmp); }
    last_parse_ms_ = now;
  }

  while (available()) {
    uint8_t b; read_byte(&b);
    last_parse_ms_ = now;
    switch (parse_state_) {
      case WAIT_HEADER:
        if (b == 0x00) {
          if (++zero_count_ >= 2) { parse_state_ = WAIT_SEP; zero_count_ = 0; }
        } else {
          zero_count_ = 0;
        }
        break;
      case WAIT_SEP:
        if (b != 0x00) {
          sep_byte_ = b; buf_pos_ = 0; parse_state_ = READ_DATA;
        }
        break;
      case READ_DATA:
        frame_buf_[buf_pos_++] = b;
        if (buf_pos_ < 4) break;
        last_msg_ms_ = now;

        if (diagnostic_mode_)
          ESP_LOGI(TAG, "DIAG [0x%02X] B0=0x%02X B1=0x%02X B2=0x%02X B3=0x%02X",
                   sep_byte_, frame_buf_[0], frame_buf_[1], frame_buf_[2], frame_buf_[3]);

        if (sep_byte_ == 0x0C) {
          // FRAME "RETE ASSENTE" (NO-AC).
          // In blackout il pannello alterna il frame di stato reale con un
          // frame-messaggio (segmento RETE spento sull'LCD). Riconoscimento su
          // B1/B2/B3 = DF 6C 5E: il B0 va IGNORATO perche' e' il display e i
          // suoi bit alti variano con chime/bypass (osservato sia D9 che 19
          // come B0 di questo frame). Univoco: il MAX ha B1=DF ma B2/B3=D6/D6.
          // Questo frame NON e' uno stato: se processato falserebbe i sensori
          // (e con B2=0x6C farebbe apparire "disarmato" un sistema armato).
          // NB sicurezza: il frame di stato reale (es. armato 5B 6C D6 5D)
          // continua ad alternarsi in blackout con B2/B3 identici al caso con
          // rete: armed/alarm restano riconosciuti. Confermato dai log.
          if (frame_buf_[1] == 0xDF && frame_buf_[2] == 0x6C && frame_buf_[3] == 0x5E) {
            last_noac_ms_ = now;                     // isteresi: rinnova il timer
            if (ac_last_published_ != 0) {           // 0 = assente
              ac_last_published_ = 0;
              if (ac_power_sensor_) ac_power_sensor_->publish_state(false);
              ESP_LOGI(TAG, "Rete 220V: ASSENTE");
            }
            // frame scartato come stato: aggiorno prev e chiudo il parsing
            memcpy(prev_frame_, frame_buf_, 4);
            prev_sep_ = sep_byte_;
            prev_frame_valid_ = true;
            parse_state_ = WAIT_HEADER; zero_count_ = 0; buf_pos_ = 0;
            break;
          }
          // NB: la rete "presente" NON si dichiara qui al primo frame normale
          // (in blackout i frame si alternano e il sensore oscillerebbe).
          // La dichiara l'isteresi nel loop: presente solo dopo 25s senza
          // frame NO-AC, col bus vivo.

          // Il frame precedente a 0x0C e' il frame di stato valido (gregrenda)
          // Applichiamo la mappatura completa del protocollo non-addressable
          if (prev_frame_valid_) {
            process_status(prev_frame_[0], prev_frame_[1], prev_frame_[2], prev_frame_[3]);
          }
          // Durante lo scan, il separatore che precede 0x0C codifica la zona
          // in scroll: zona = (0xFC - sep) / 8. Lo memorizzo qui, perche'
          // subito dopo arriva il frame display 0x04 5E FC FC.
          if (scanning_zones_) pending_zone_sep_ = prev_sep_;
          memcpy(pending_frame_, frame_buf_, 4);
          pending_frame_valid_ = true;
          frame04_count_ = 0;
          partial_seen_ = false;
        } else if (sep_byte_ == 0x04) {
          // Durante lo scan, al frame display 0x04 5E FC FC decodifico la zona
          // dal separatore catturato prima del 0x0C (sorgente NON ambigua,
          // a differenza del B3 dove 1/2/4 valgono tutte 0x7E).
          if (scanning_zones_ &&
              frame_buf_[0] == 0x5E && frame_buf_[1] == 0xFC && frame_buf_[2] == 0xFC) {
            uint8_t sep = pending_zone_sep_;
            if (sep >= 0xBC && sep <= 0xF4 && ((0xFC - sep) % 8) == 0) {
              uint8_t z = (0xFC - sep) / 8;          // 0xF4->1 ... 0xBC->8
              if (z >= 1 && z <= 8) {
                uint16_t bit = 1 << (z - 1);
                if (!(scan_zones_mask_ & bit)) {
                  scan_zones_mask_ |= bit;
                  ESP_LOGI(TAG, "Zona aperta: %d (sep=0x%02X B3=0x%02X mask=0x%02X)",
                           z, sep, frame_buf_[3], scan_zones_mask_);
                }
              }
            } else {
              ESP_LOGW(TAG, "Scan: separatore zona non valido 0x%02X (B3=0x%02X)",
                       sep, frame_buf_[3]);
            }
          }
          if (++frame04_count_ == 2 && pending_frame_valid_) {
            partial_seen_ = (frame_buf_[2] & 0x40) != 0;
            process_armed(pending_frame_[0], pending_frame_[1],
                          pending_frame_[2], pending_frame_[3]);
            pending_frame_valid_ = false;
          }
        }

        // Salva il frame corrente come precedente
        memcpy(prev_frame_, frame_buf_, 4);
        prev_sep_ = sep_byte_;
        prev_frame_valid_ = true;

        parse_state_ = WAIT_HEADER; zero_count_ = 0; buf_pos_ = 0;
        break;
    }
  }

  // Chiusura a tempo FISSO: 14s = ~1,5 cicli di display, abbastanza per
  // vedere tutte le zone in scroll. A questo punto il '*' e' stato trasmesso
  // da un pezzo, quindi l'uscita (codice+1) parte davvero e il pannello esce.
  if (scanning_zones_ && (now - scan_start_ms_) > 14000) {
    finish_zone_scan();
  }

  // Isteresi rete 220V. In blackout il frame NO-AC riappare al massimo
  // ogni ~10s (misurato dai log): se non lo vediamo da 25s e il bus e' vivo,
  // la rete e' tornata (o c'e' sempre stata). Cosi' il sensore non oscilla
  // con l'alternanza dei frame durante il blackout.
  if (ac_last_published_ != 1 &&
      (now - last_noac_ms_) > 25000 &&
      last_msg_ms_ != 0 && (now - last_msg_ms_) < 5000) {
    ac_last_published_ = 1;
    if (ac_power_sensor_) ac_power_sensor_->publish_state(true);
    ESP_LOGI(TAG, "Rete 220V: PRESENTE");
  }

  static uint8_t last_ki = 0;
  if (key_idx_ != last_ki) {
    ESP_LOGD(TAG, "TX tasto [%d/%d]", (int)key_idx_, (int)(num_keys_ ? num_keys_ : key_idx_));
    last_ki = key_idx_;
    if (!num_keys_ && !key_idx_ && last_ki) { ESP_LOGI(TAG, "Seq. completata"); last_ki = 0; }
  }

  static uint32_t last_warn = 0;
  if (last_msg_ms_ && (now - last_msg_ms_) > 15000 && (now - last_warn) > 15000) {
    ESP_LOGW(TAG, "Bus silenzioso da %ds", (now - last_msg_ms_) / 1000);
    last_warn = now;
  }
}

// ─────────────────────────────────────────────────────────────
//  PROCESS STATUS — frame precedente a 0x0C (separatore 0x24, B0=0xF9)
//  Mappatura confermata empiricamente sul 4120:
//  ready:  B2 bit1 0x02 attivo basso (0=pronto, 1=non pronto)
//  bypass: B2 bit7 0x80 attivo basso (0=bypass attivo)
//  chime:  B3 bit1 0x02 — INVERTITO quando bypass attivo
//  Tabella di verita' verificata:
//    Senza bypass, chime ON:  B2=0xFC B3=0xFE
//    Senza bypass, chime OFF: B2=0xFC B3=0xFC
//    Con bypass,   chime ON:  B2=0x7C B3=0xFC
//    Con bypass,   chime OFF: B2=0x7C B3=0xFE
// ─────────────────────────────────────────────────────────────
void Ademco4110Component::process_status(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
  // In blackout la sequenza dei frame cambia (il frame prima dello 0x0C
  // puo' essere un 0x24 invece del solito) e il prev_frame non e' affidabile
  // per chime/ready/bypass. Li CONGELIAMO all'ultimo valore noto finche' la
  // rete non torna (l'isteresi li sblocca). armed/alarm NON passano di qui
  // e restano sempre attivi (process_armed), come richiesto per sicurezza.
  if (ac_last_published_ == 0) {
    ESP_LOGD(TAG, "STATUS congelato (NO-AC) B2=0x%02X B3=0x%02X", b2, b3);
    return;
  }

  bool ready  = (b2 & 0x02) == 0;  // attivo basso
  bool bypass = (b2 & 0x80) == 0;  // attivo basso
  bool chime_bit = (b3 & 0x02) != 0;
  bool chime  = bypass ? !chime_bit : chime_bit;  // invertito col bypass

  // Batteria scarica — IPOTESI da gregrenda (Byte2 bit6 BAT), attivo basso
  // DA VERIFICARE staccando la batteria tampone: se invertito, cambiare == in !=
  bool battery_low = (b2 & 0x40) == 0;

  ESP_LOGD(TAG, "STATUS B2=0x%02X B3=0x%02X | ready=%d chime=%d bypass=%d bat_low=%d",
           b2, b3, ready, chime, bypass, battery_low);

  if (ready_sensor_ && system_state_ == STATE_DISARMED)
                         ready_sensor_->publish_state(ready);
  if (chime_sensor_)     chime_sensor_->publish_state(chime);
  if (bypass_sensor_)    bypass_sensor_->publish_state(bypass);
  if (battery_low_sensor_) battery_low_sensor_->publish_state(battery_low);
}

// ─────────────────────────────────────────────────────────────
//  PROCESS ARMED — frame 0x0C + secondo 0x04
//  armed: B2 bit7 0x80
//  alarm: B3 bit1 0x02
//  partial: frame 0x04 bit6
// ─────────────────────────────────────────────────────────────
void Ademco4110Component::process_armed(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
  bool armed = (b2 & 0x80) != 0;
  // L'allarme è B3 bit1 (0x02), MA solo se NON è attivo B3 bit7 (0x80).
  // In modalità MAX il pannello alza B3 bit1 senza essere in allarme (frame 0x0C
  // armato = 0x19 0xDF 0xD6 0xD6, B3=0xD6 → bit1=1 bit7=1). Un allarme VERO da
  // qualsiasi stato (parziale/totale/immediato/MAX) produce sempre il frame
  // 0x1F 0x16 0xDF 0x2B (B3=0x2B → bit1=1 bit7=0). Quindi bit7 distingue MAX
  // (armato, non allarme) dall'allarme reale, senza mai sopprimere un allarme.
  bool alarm = ((b3 & 0x02) != 0) && ((b3 & 0x80) == 0);

  ESP_LOGD(TAG, "ARMED B0=0x%02X B1=0x%02X B2=0x%02X B3=0x%02X | armed=%d alarm=%d partial=%d",
           b0, b1, b2, b3, armed, alarm, partial_seen_);

  // Alla transizione false→true dell'allarme, apri la finestra di voto.
  // Durante la finestra accumuliamo i separatori e poi pubblichiamo la zona
  // più frequente (robusto contro separatori spuri del ciclo display d'allarme).
  static bool prev_alarm = false;
  if (alarm && !prev_alarm && armed && alarm_zone_sensor_) {
    alarm_voting_ = true;
    alarm_vote_start_ms_ = millis();
    memset(alarm_zone_votes_, 0, sizeof(alarm_zone_votes_));
    alarm_first_zone_ = 0;
    // conta subito il separatore corrente
    alarm_vote_separator(prev_sep_);
    ESP_LOGI(TAG, "Allarme: avvio votazione zona (finestra %ums)", ALARM_VOTE_MS);
  } else if (alarm && alarm_voting_) {
    // durante l'allarme, continua ad accumulare i separatori
    alarm_vote_separator(prev_sep_);
    if (millis() - alarm_vote_start_ms_ >= ALARM_VOTE_MS) {
      finish_alarm_vote();
    }
  } else if (!alarm && alarm_voting_) {
    // allarme cessato prima dello scadere della finestra: chiudi col raccolto
    finish_alarm_vote();
  }
  prev_alarm = alarm;

  if (alarm && armed)              set_system_state(STATE_ALARM);
  else if (armed && partial_seen_) set_system_state(STATE_ARMED_PARTIAL);
  else if (armed)                  set_system_state(STATE_ARMED_TOTAL);
  else                             set_system_state(STATE_DISARMED);
}

void Ademco4110Component::set_system_state(SystemState s) {
  if (s == system_state_) return;
  ESP_LOGI(TAG, "Stato: %s -> %s", state_str(system_state_), state_str(s));
  system_state_ = s;
  publish_all();
}

void Ademco4110Component::publish_all() {
  bool a = system_state_ == STATE_ARMED_TOTAL ||
           system_state_ == STATE_ARMED_PARTIAL ||
           system_state_ == STATE_ALARM;
  if (armed_sensor_)         armed_sensor_->publish_state(a);
  if (armed_total_sensor_)   armed_total_sensor_->publish_state(system_state_ == STATE_ARMED_TOTAL);
  if (armed_partial_sensor_) armed_partial_sensor_->publish_state(system_state_ == STATE_ARMED_PARTIAL);
  if (alarm_sensor_)         alarm_sensor_->publish_state(system_state_ == STATE_ALARM);
  if (status_sensor_)        status_sensor_->publish_state(state_str(system_state_));
  // Azzera zona allarme quando il sistema è disarmato
  if (system_state_ == STATE_DISARMED && alarm_zone_sensor_) {
    alarm_zone_sensor_->publish_state("");
  }
  // Ready non viene toccato quando inserito — resta all'ultimo valore noto
}

const char *Ademco4110Component::state_str(SystemState s) {
  switch (s) {
    case STATE_DISARMED:      return "disarmed";
    case STATE_ARMED_TOTAL:   return "armed_total";
    case STATE_ARMED_PARTIAL: return "armed_partial";
    case STATE_ALARM:         return "alarm";
    default:                  return "unknown";
  }
}

// ─────────────────────────────────────────────────────────────
//  SCANSIONE ZONE APERTE
//  Manda '*', raccoglie i separatori zona (modalita' display zone),
//  calcola le zone con formula zona = (0xFC - sep) / 0x08,
//  poi esce mandando codice+1 (OFF).
// ─────────────────────────────────────────────────────────────
void Ademco4110Component::scan_zones() {
  if (scanning_zones_) { ESP_LOGW(TAG, "Scansione gia' in corso"); return; }
  if (disarm_code_.empty()) {
    ESP_LOGW(TAG, "disarm_code non configurato");
    return;
  }
  ESP_LOGI(TAG, "Avvio scansione zone aperte");
  scanning_zones_ = true;
  scan_zones_mask_ = 0;
  scan_start_ms_ = millis();
  scan_last_zone_ms_ = millis();
  if (zones_sensor_) zones_sensor_->publish_state("scansione...");
  send_keys("*");
}

void Ademco4110Component::finish_zone_scan() {
  scanning_zones_ = false;

  std::string result;
  for (uint8_t z = 1; z <= 8; z++) {
    if (scan_zones_mask_ & (1 << (z - 1))) {
      if (!result.empty()) result += ", ";
      result += ZONE_NAMES[z];
    }
  }
  if (result.empty()) result = "nessuna";

  ESP_LOGI(TAG, "Scansione completata. Zone aperte: %s", result.c_str());
  if (zones_sensor_) zones_sensor_->publish_state(result);

  std::string off_seq = disarm_code_ + "1";
  send_keys(off_seq.c_str());
}

// Accumula un voto per la zona codificata nel separatore, se valido.
void Ademco4110Component::alarm_vote_separator(uint8_t sep) {
  if (sep < 0xBC || sep > 0xF4) return;
  if (((0xFC - sep) % 8) != 0) return;
  uint8_t z = (0xFC - sep) / 8;
  if (z < 1 || z > 8) return;
  if (alarm_zone_votes_[z] < 255) alarm_zone_votes_[z]++;
  if (alarm_first_zone_ == 0) alarm_first_zone_ = z;  // prima zona vista (tie-break)
}

// Chiude la finestra di voto e pubblica la zona col conteggio più alto.
// In caso di parità vince la prima vista. Robusto contro separatori spuri.
void Ademco4110Component::finish_alarm_vote() {
  alarm_voting_ = false;
  uint8_t best_zone = 0;
  uint8_t best_count = 0;
  for (uint8_t z = 1; z <= 8; z++) {
    if (alarm_zone_votes_[z] > best_count) {
      best_count = alarm_zone_votes_[z];
      best_zone = z;
    }
  }
  // tie-break: se la prima zona vista pareggia col vincitore, preferiscila
  if (alarm_first_zone_ >= 1 && alarm_first_zone_ <= 8 &&
      alarm_zone_votes_[alarm_first_zone_] == best_count) {
    best_zone = alarm_first_zone_;
  }
  if (best_zone >= 1 && best_zone <= 8 && alarm_zone_sensor_) {
    ESP_LOGI(TAG, "Allarme zona (voto): %s [%d voti, prima=%s]",
             ZONE_NAMES[best_zone], best_count,
             alarm_first_zone_ ? ZONE_NAMES[alarm_first_zone_] : "-");
    alarm_zone_sensor_->publish_state(ZONE_NAMES[best_zone]);
  }
}

uint8_t Ademco4110Component::char_to_key(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c == '*') return 0x0A;
  if (c == '#') return 0x0B;
  return 0xFF;
}

void Ademco4110Component::send_keys(const char *keys) {
  if (num_keys_) { ESP_LOGW(TAG, "TX in corso"); return; }
  num_keys_ = 0; key_idx_ = 0;
  for (int i = 0; keys[i] && num_keys_ < 10; i++) {
    uint8_t k = char_to_key(keys[i]);
    if (k != 0xFF) keys_[num_keys_++] = k;
  }
  if (num_keys_) ESP_LOGI(TAG, "Coda: %d tasti", (int)num_keys_);
}

}  // namespace ademco4110
}  // namespace esphome
