#include "ademco4110.h"
#include "esphome/core/log.h"
#include <esp_timer.h>
#include <esp_rom_gpio.h>
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <string.h>
#include <string>

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
  // Porting da driver/rmt.h (deprecato/rimosso) a driver/rmt_tx.h.
  // Stessa risoluzione (1 tick = 1us) e stesso comportamento bloccante
  // dell'implementazione precedente; cambia solo l'API sottostante.
  rmt_tx_channel_config_t cfg = {};
  cfg.gpio_num = TX_GPIO;
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = RMT_RESOLUTION_HZ;
  cfg.mem_block_symbols = 64;
  cfg.trans_queue_depth = 1;
  cfg.flags.invert_out = false;
  cfg.flags.with_dma = false;
  ESP_ERROR_CHECK(rmt_new_tx_channel(&cfg, &rmt_tx_chan_));

  rmt_copy_encoder_config_t enc_cfg = {};
  ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, &rmt_copy_encoder_));

  ESP_ERROR_CHECK(rmt_enable(rmt_tx_chan_));
  ESP_LOGI(TAG, "RMT TX inizializzato su GPIO%d", (int)TX_GPIO);
}

void Ademco4110Component::rmt_tx_send_key(uint8_t key) {
  rmt_symbol_word_t items[15];
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
  // La nuova API non richiede un item terminatore: la lunghezza esplicita
  // in byte sostituisce la convenzione "item a zero = fine sequenza".
  rmt_transmit_config_t tx_config = {};
  tx_config.loop_count = 0;
  tx_config.flags.eot_level = 1;  // linea idle-high a fine trasmissione (come RMT_IDLE_LEVEL_HIGH)
  ESP_ERROR_CHECK(rmt_transmit(rmt_tx_chan_, rmt_copy_encoder_, items,
                                idx * sizeof(rmt_symbol_word_t), &tx_config));
  ESP_ERROR_CHECK(rmt_tx_wait_all_done(rmt_tx_chan_, portMAX_DELAY));
}

void Ademco4110Component::setup() {
  // rmt_tx_init() NON viene chiamato qui: setup() gira sul task principale di
  // ESPHome, pinnato al Core 1, mentre sync_task() (che poi usa/attende il
  // canale RMT) e' pinnato al Core 0. Creare il canale su un core e attenderlo
  // dall'altro e' un pattern a rischio con la nuova API RMT (visto in campo:
  // rmt_tx_wait_all_done() si blocca dalla seconda trasmissione in poi).
  // Init spostata dentro sync_task(), cosi' canale RMT e attesa vivono sempre
  // sullo stesso core.
  if (sync_pin_) {
    sync_pin_->setup();
    sync_pin_->pin_mode(gpio::FLAG_INPUT);
    xTaskCreatePinnedToCore(sync_task, "sync_task", 2048, this, 24, nullptr, 0);
  }
  system_state_ = STATE_UNKNOWN;
  publish_all();
  if (bus_ok_sensor_) bus_ok_sensor_->publish_state(true);  // stato iniziale ottimistico
  if (zone_changed_sensor_) zone_changed_sensor_->publish_state("nessuna");
}

void Ademco4110Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Ademco 4120 v15g (voto zona allarme):");
  ESP_LOGCONFIG(TAG, "  TX GPIO: %d", (int)TX_GPIO);
  ESP_LOGCONFIG(TAG, "  sync_pin: %s", sync_pin_ ? "OK" : "non configurato");
  ESP_LOGCONFIG(TAG, "  diagnostic_mode: %s", diagnostic_mode_ ? "ON" : "off");
}

void Ademco4110Component::sync_task(void *arg) {
  auto *self = static_cast<Ademco4110Component *>(arg);
  self->rmt_tx_init();  // canale RMT creato qui: stesso core (0) che poi lo usa
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
          // Il separatore che precede questo 0x0C (prev_sep_, gia' fissato dal
          // frame letto appena prima) va controllato QUI, prima di qualsiasi
          // filtro sotto che puo' uscire in anticipo (NO-AC, evento bypass):
          // altrimenti proprio nei casi che ci interessano (l'evento bypass
          // segue sempre il separatore zona) non verrebbe mai raggiunto.
          if (scanning_zones_) {
            // Durante lo scan, il separatore codifica la zona in scroll:
            // zona = (0xFC - sep) / 8. Lo memorizzo qui, perche' subito dopo
            // arriva il frame display 0x04 5E FC FC.
            pending_zone_sep_ = prev_sep_;
          } else if (alarm_voting_) {
            // Durante la finestra di voto allarme, il separatore-zona (stesso
            // meccanismo del bypass) va accumulato come voto qui, non trattato
            // come modifica bypass. Prima veniva letto (sbagliato) dentro
            // process_armed() durante il secondo frame 0x04, dove prev_sep_ e'
            // ormai 0x04 e non il separatore zona - mai un voto valido.
            // Confermato su un log reale di allarme: separatore 0xDC (zona 4)
            // comparso esattamente prima dello 0x0C che porta ad alarm=1.
            alarm_vote_separator(prev_sep_);
          } else if (frame_buf_[1] == 0xD6 && frame_buf_[2] == 0x5E && frame_buf_[3] == 0x16 &&
                     prev_sep_ >= 0xBC && prev_sep_ <= 0xF4 && ((0xFC - prev_sep_) % 8) == 0) {
            // BYPASS VERO: il separatore-zona (0xDC, 0xCC, ...) DEVE essere
            // seguito dal frame "evento bypass" D6 5E 16. Verificato sui log:
            // bypass reale di zona 4 -> [0xDC][0x0C D6 5E 16]. FONDAMENTALE:
            // lo stesso separatore-zona compare anche prima del frame CD 17 2B
            // (uno stato transitorio del pannello, NON un bypass) e prima causava
            // falsi (zone che comparivano/ciclavano da sole). Richiedendo il
            // frame D6 5E 16 distinguiamo il bypass reale da quel transitorio.
            uint8_t z = (0xFC - prev_sep_) / 8;
            if (z >= 1 && z <= 8) {
              // Toggle sulla maschera: coerente con l'uso del bypass (digiti la
              // zona per alternarne lo stato). La maschera si azzera comunque
              // alla disattivazione del bypass (vedi process_status).
              bypass_zone_mask_ ^= (1 << (z - 1));
              ESP_LOGI(TAG, "Zona modificata (bypass): %s (mask=0x%02X)", ZONE_NAMES[z], bypass_zone_mask_);
              publish_bypass_zones();
            }
          }

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

          // FRAME "EVENTO BYPASS ZONA" (beep di conferma esclusione/inclusione).
          // Osservato identico durante l'esclusione zona 4 e zona 6: B1/B2/B3 =
          // D6 5E 16. B0 ignorato per coerenza col trattamento NO-AC (non ancora
          // confermato se vari). NON e' un frame di stato: processato in
          // process_armed() calcolerebbe alarm=true (qui mascherato solo perche'
          // armed risultava false in quel frame — non e' garantito in generale,
          // quindi va scartato esplicitamente come il NO-AC).
          if (frame_buf_[1] == 0xD6 && frame_buf_[2] == 0x5E && frame_buf_[3] == 0x16) {
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

          // Lo stato (chime/pronto/bypass/batteria) si legge dal frame che
          // precede lo 0x0C, MA solo se quel frame e' il vero frame di stato.
          // Firma univoca verificata sui log: il frame di stato reale ha
          // sempre B0=0xF9 (F9 DE FE FE, F9 FC 3E FC, ...). TUTTI i frame che
          // davano letture false hanno B0 diverso: 0xBE (frame display, es.
          // BE FE 3C B3 -> pronto/bypass/batteria falsi), 0xFC/0xFE (frame
          // separatore zona), o il primo di due 0x0C consecutivi (B0=D6/1F).
          // Leggere solo da B0=0xF9 sostituisce i vecchi filtri caso-per-caso
          // (BE/00, doppio-0x0C) con un'unica firma positiva, piu' robusta.
          // Fallimento sicuro: se mai uno stato non visto avesse B0!=F9, i
          // sensori restano all'ultimo valore (fermi, mai falsi); armed/alarm
          // non passano di qui (process_armed), quindi la sicurezza e' intatta.
          if (prev_frame_valid_ && prev_frame_[0] == 0xF9) {
            process_status(prev_frame_[0], prev_frame_[1], prev_frame_[2], prev_frame_[3]);
          }
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
    ESP_LOGW(TAG, "Bus silenzioso da %ds", (int)((now - last_msg_ms_) / 1000));
    last_warn = now;
  }

  // Sensore diagnostico "Comunicazione Bus": pubblica solo al cambio di stato.
  // Soglia piu' larga del warning sopra (15s): quello e' solo un avviso nel
  // log, questo pilota un'entita' in HA e non deve sfarfallare per brevi
  // interruzioni normali del bus, solo per un guasto reale e prolungato.
  bool bus_silent = last_msg_ms_ && (now - last_msg_ms_) > BUS_OK_SILENCE_MS;
  if (bus_silent != bus_silent_) {
    bus_silent_ = bus_silent;
    if (bus_ok_sensor_) bus_ok_sensor_->publish_state(!bus_silent_);
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

  // Il bypass e' tornato disattivo: azzera la maschera e pulisci il campo
  // "Zona Bypass Modificata". Finche' il bypass resta attivo il campo resta
  // aggiornato dalla rilevazione passiva in loop(); si azzera solo alla
  // transizione ON->OFF, non ad ogni frame con bypass gia' spento — e questo
  // azzeramento e' anche la rete di sicurezza contro eventuali disallineamenti
  // della maschera (il toggle da solo non potrebbe mai autocorreggersi).
  if (!bypass && bypass_prev_) {
    bypass_zone_mask_ = 0;
    if (zone_changed_sensor_) zone_changed_sensor_->publish_state("nessuna");
  }
  bypass_prev_ = bypass;
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
    // Il voto vero e proprio viene accumulato in loop(), nello stesso punto
    // dove si rileva il separatore-zona per il bypass (prev_sep_ qui sarebbe
    // gia' 0x04, non il separatore zona: leggerlo qui non ha mai funzionato).
    ESP_LOGI(TAG, "Allarme: avvio votazione zona (finestra %ums)", (unsigned)ALARM_VOTE_MS);
  } else if (alarm && alarm_voting_) {
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

void Ademco4110Component::publish_bypass_zones() {
  if (!zone_changed_sensor_) return;
  std::string result;
  for (uint8_t z = 1; z <= 8; z++) {
    if (bypass_zone_mask_ & (1 << (z - 1))) {
      if (!result.empty()) result += ", ";
      result += ZONE_NAMES[z];
    }
  }
  if (result.empty()) result = "nessuna";
  zone_changed_sensor_->publish_state(result);
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
