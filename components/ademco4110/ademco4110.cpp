#include "ademco4110.h"
#include "esphome/core/log.h"
#include <esp_timer.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>   // esp_rom_delay_us (offset fine TX deterministica)
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <string.h>
#include <string>

namespace esphome {
namespace ademco4110 {

static const char *TAG = "ademco4120";

// ─────────────────────────────────────────────────────────────
//  Mappatura bit CONFERMATA EMPIRICAMENTE sul 4120+4127.
//  ATTENZIONE: NON coincide con le posizioni di gregrenda (valide per
//  pannelli/tastiere diversi) — qui valgono quelle misurate sul nostro ferro.
//
//  Frame di STATO (separatore 0x24, B0=0xF9, precede lo 0x0C):
//    B2 bit1 (0x02) = READY    (attivo basso: 0 = pronto)
//    B2 bit6 (0x40) = BATTERIA (attivo basso; falso positivo col bypass -> soppresso)
//    B2 bit7 (0x80) = BYPASS   (attivo basso: 0 = bypass attivo)
//    B3 bit1 (0x02) = CHIME    (bit di display: invertito con bypass e armato-totale -> compensato in XOR)
//
//  Frame ARMATO/ALLARME (0x0C):
//    B2 bit7 (0x80) = ARMED
//    B3 bit1 (0x02) = ALARM    (solo se B3 bit7==0; bit7=1 = modo MAX, non allarme)
//
//  PARZIALE: 2o frame 0x04 dopo lo 0x0C, B2 bit6 (0x40).
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
  // Attesa limitata invece di portMAX_DELAY: se il canale RMT si pianta (visto in
  // campo, vedi commento in setup()) l'attesa infinita congelava sync_task per
  // sempre, e con key_sending_ bloccato a true num_keys_ non tornava mai a zero:
  // da li' in poi ogni send_keys() usciva con "TX in corso" e la tastiera moriva
  // in silenzio. Niente ESP_ERROR_CHECK: un timeout va loggato, non deve far
  // abortire la scheda.
  esp_err_t err = rmt_tx_wait_all_done(rmt_tx_chan_, pdMS_TO_TICKS(RMT_TX_TIMEOUT_MS));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "RMT TX non completata: %s", esp_err_to_name(err));
  }
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
    if (deterministic_tx_) {
      // TX deterministica: coda tasti + tx_task ad alta priorita' svegliato da un
      // ISR sul sync pin. A differenza del vecchio busy-loop a prio 24 (che
      // affamava il WiFi girando a ogni tick), qui il task e' BLOCCATO su una
      // notifica: 0% CPU a riposo, si sveglia solo quando c'e' un tasto in coda a
      // un impulso di sync. L'allineamento fine alla finestra avviene da t_sync
      // catturato nell'ISR -> lo scheduler e' fuori dal percorso del trigger.
      // Stack 4096 come sync_task (chiama rmt_tx_init()). L'ISR la installa
      // ESPHome (attach_interrupt): e' core-agnostica, tocca solo timestamp+notify.
      key_queue_ = xQueueCreate(16, sizeof(uint8_t));
      xTaskCreatePinnedToCore(tx_task, "tx_task", 4096, this, 24, &tx_task_handle_, 0);
      sync_pin_->attach_interrupt(&Ademco4110Component::sync_isr, this,
                                  gpio::INTERRUPT_ANY_EDGE);
    } else {
      // Stack 4096: il task chiama rmt_tx_init() (driver RMT dell'IDF) e ESP_LOGI,
      // catena che con 2048 byte era al limite dell'overflow -> panic e reboot.
      // Priorita' 10 (era 24): sul core 0 girano il task WiFi (23) e lo stack
      // TCP/IP (18); a 24 il sync_task li scavalcava e affamava la rete -> timeout
      // API. A 10 resta ben sopra il loop ESPHome (prio 1), quindi i tasti partono
      // comunque in fretta, ma non preempta piu' la rete: cosi' si possono togliere
      // i timeout API SENZA tenere la radio WiFi sempre accesa (power_save: light).
      xTaskCreatePinnedToCore(sync_task, "sync_task", 4096, this, 10, nullptr, 0);
    }
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
  ESP_LOGCONFIG(TAG, "  raw_dump: %s", raw_dump_ ? "ON" : "off");
  ESP_LOGCONFIG(TAG, "  parser: %s", new_parser_ ? "NUOVO (marcatori+debounce)" : "vecchio (2 zeri)");
  ESP_LOGCONFIG(TAG, "  TX: %s", deterministic_tx_ ? "deterministica (ISR+task prio24)" : "sync_task (polling prio10)");
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

// ─────────────────────────────────────────────────────────────
//  TX DETERMINISTICA (deterministic_tx: true)
//  sync_isr: aggancia il sync col MEDESIMO criterio del polling
//  (diff > SYNC_MIN_US), ma via interrupt (latenza ~0, niente jitter di
//  scheduling). Non chiama l'RMT (non ISR-safe): passa solo l'istante hardware
//  t_sync nella notifica e sveglia tx_task. isr_last_edge_us_ e' toccato solo
//  qui (un core) -> nessun lock. La coda e' l'unica sorgente di "c'e' da
//  trasmettere": se e' vuota non si notifica e il task resta a dormire.
// ─────────────────────────────────────────────────────────────
void IRAM_ATTR Ademco4110Component::sync_isr(Ademco4110Component *self) {
  int64_t now = esp_timer_get_time();
  int64_t diff = now - self->isr_last_edge_us_;
  self->isr_last_edge_us_ = now;
  if (diff > (int64_t) SYNC_MIN_US) {
    self->isr_sync_count_++;   // diag: conteggio impulsi di sync (vivo/fermo in idle)
    if (uxQueueMessagesWaitingFromISR(self->key_queue_) > 0) {
      BaseType_t hpw = pdFALSE;
      // t_sync a 32 bit nel valore di notifica: lettura atomica lato task
      // (niente torn-read di un int64) e sempre l'ultimo sync se ne arriva un altro.
      xTaskNotifyFromISR(self->tx_task_handle_, (uint32_t) now,
                         eSetValueWithOverwrite, &hpw);
      portYIELD_FROM_ISR(hpw);
    }
  }
}

// tx_task: prio 24 (sopra il WiFi) ma BLOCCATO su xTaskNotifyWait -> 0% CPU
// finche' non c'e' un sync con tasti in coda. Si sveglia, si riallinea con
// precisione all'istante hardware (residuo dei TX_OFFSET_US via busy-wait
// cortissimo, solo mentre invia), poi trasmette. rmt_tx_send_key resta identico
// (3x burst) e la sua attesa fine-TX rilascia la CPU al WiFi durante gli ~11ms
// di clock hardware, quindi il core 0 e' trattenuto solo per l'offset.
void Ademco4110Component::tx_task(void *arg) {
  auto *self = static_cast<Ademco4110Component *>(arg);
  self->rmt_tx_init();  // canale RMT creato/usato sullo stesso core (0), come sync_task
  int64_t last_key_us = -(int64_t) MIN_KEY_GAP_US;  // il primo tasto parte subito
  for (;;) {
    uint32_t t_sync_lo = 0;
    xTaskNotifyWait(0, 0xFFFFFFFFUL, &t_sync_lo, portMAX_DELAY);
    // Ritmo minimo tra tasti: il pannello si impalla se arrivano piu' veloci di
    // ~0.5s (indicazione del tecnico, confermata dai log: raffiche a 0.2s
    // mandavano il display in churn -> sync affamato -> stallo -> a volte codice
    // scartato). Se non e' passato abbastanza, salta questo sync: il tasto resta
    // in coda e l'ISR ci risveglia al prossimo (pacing naturale sui sync).
    if (esp_timer_get_time() - last_key_us < (int64_t) MIN_KEY_GAP_US)
      continue;
    uint8_t key;
    if (xQueueReceive(self->key_queue_, &key, 0) == pdTRUE) {
      uint32_t elapsed = (uint32_t) esp_timer_get_time() - t_sync_lo;
      if (elapsed < TX_OFFSET_US)
        esp_rom_delay_us(TX_OFFSET_US - elapsed);
      self->rmt_tx_send_key(key);
      last_key_us = esp_timer_get_time();
      self->last_tx_ms_ = millis();
      if (self->diagnostic_mode_)
        ESP_LOGI(TAG, "TX 0x%02X elapsed=%uus coda=%d sync=%u", key,
                 (unsigned) elapsed,
                 (int) uxQueueMessagesWaiting(self->key_queue_),
                 (unsigned) self->isr_sync_count_);
    }
  }
}

// ─────────────────────────────────────────────────────────────
//  RAW DUMP — byte grezzi dal bus, PRIMA del parsing (opt-in raw_dump: true).
//  Affianca il DIAG, non lo sostituisce: una riga per frame, allineata al
//  DIAG corrispondente, per confrontarle riga per riga e vedere se uno 0x00
//  spurio (impulso di sync/inter-byte) e' finito dentro i 4 byte dati.
//  NB: le DURATE degli impulsi non sono qui. La RX e' via UART, che decodifica
//  i byte e scarta il timing inter-byte (l'RMT e' solo TX). Catturare gli
//  impulsi da 1/6/17ms richiederebbe un canale RMT RX o un ISR sul pin bus:
//  intervento separato, non incluso in questa modalita'.
// ─────────────────────────────────────────────────────────────
void Ademco4110Component::raw_flush() {
  if (raw_len_ == 0) return;
  char hex[3 * sizeof(raw_buf_) + 1];
  size_t p = 0;
  for (uint8_t i = 0; i < raw_len_; i++)
    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", raw_buf_[i]);
  if (p > 0) hex[p - 1] = '\0';  // togli lo spazio finale
  ESP_LOGI(TAG, "RAW [t=%llu] len=%u : %s",
           (unsigned long long) raw_first_us_, raw_len_, hex);
  raw_len_ = 0;
}

// Marcatori validi noti sul bus: 0x24/0x0C/0x04 + separatori zona, cioe' i sep
// con (0xFC - sep)/8 in 1..8 -> 0xF4,0xEC,0xE4,0xDC,0xD4,0xCC,0xC4,0xBC.
// Usato solo dal nuovo framer (new_parser) per agganciare l'inizio del frame.
static bool is_marker(uint8_t b) {
  if (b == 0x04 || b == 0x0C || b == 0x24) return true;
  if (b >= 0xBC && b <= 0xF4 && ((0xFC - b) % 8) == 0) return true;
  return false;
}

void Ademco4110Component::loop() {
  uint32_t now = millis();

  if (parse_state_ != WAIT_HEADER && last_parse_ms_ && (now - last_parse_ms_) > 2000) {
    ESP_LOGW(TAG, "Parser reset");
    if (raw_dump_) raw_flush();
    parse_state_ = WAIT_HEADER;
    zero_count_ = 0;
    buf_pos_ = 0;
    while (available()) { uint8_t tmp; read_byte(&tmp); }
    last_parse_ms_ = now;
  }

  while (available()) {
    uint8_t b; read_byte(&b);
    if (raw_dump_) {  // cattura grezza PRIMA di qualsiasi elaborazione
      if (raw_len_ >= sizeof(raw_buf_)) raw_flush();   // buffer pieno: dump e riparti
      if (raw_len_ == 0) raw_first_us_ = esp_timer_get_time();
      raw_buf_[raw_len_++] = b;
    }
    last_parse_ms_ = now;
    switch (parse_state_) {
      case WAIT_HEADER:
        if (new_parser_) {
          // Nuovo framer: aggancia sul MARCATORE valido, saltando zeri E byte-
          // spazzatura. Cattura anche i frame impacchettati (senza i 2 zeri di
          // header) che il vecchio parser perde in raffica. Una spazzatura che
          // coincide con un marcatore diventa un frame-fantasma isolato, filtrato
          // a valle dal debounce su armed/alarm (in process_armed).
          if (b != 0x00 && is_marker(b)) {
            sep_byte_ = b; buf_pos_ = 0; parse_state_ = READ_DATA;
          }
        } else if (b == 0x00) {
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

        if (raw_dump_)
          raw_flush();  // riga RAW del frame appena completato, allineata al DIAG sotto
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

  // Diagnostica TX deterministica: se restano tasti in coda che non partono,
  // logga quanti sono in attesa + il conteggio impulsi di sync. Confrontando 'sync'
  // tra righe consecutive mentre i tasti sono fermi: se CRESCE -> il sync e' vivo
  // (problema di timing/pannello); se resta FERMO -> il sync si e' fermato in idle
  // (tastiera addormentata, serve svegliarla). Risponde all'incognita sync-in-idle.
  if (diagnostic_mode_ && deterministic_tx_ && key_queue_) {
    static uint32_t last_stall_log = 0;
    UBaseType_t pending = uxQueueMessagesWaiting(key_queue_);
    if (pending > 0 && (now - last_tx_ms_) > 500 && (now - last_stall_log) > 1000) {
      ESP_LOGW(TAG, "coda TX ferma: %d in attesa, sync=%u, bus %dms fa",
               (int) pending, (unsigned) isr_sync_count_, (int)(now - last_msg_ms_));
      last_stall_log = now;
    }
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
  // Il bit del chime (B3 bit1) e' un bit di DISPLAY: il pannello lo INVERTE
  // quando il display e' in modo "speciale" — bypass attivo OPPURE armato TOTALE
  // (AWAY). NON lo inverte da parziale (STAY) ne' da disinserito. Verificato sui
  // frame reali: disarmato/parziale chime-on -> bit1=1; armato-totale chime-off
  // -> bit1=1 (invertito); bypass -> invertito. Doppia compensazione con XOR.
  // (Cosmetici non testati: stato allarme, e armato-totale+bypass insieme.)
  bool armed_total = system_state_ == STATE_ARMED_TOTAL;
  bool chime = chime_bit ^ bypass ^ armed_total;

  // Batteria scarica — Byte2 bit6 (attivo basso). ATTENZIONE: il bit6 si abbassa
  // anche col bypass attivo + sistema non pronto (osservato B2=0x3E: bit6=0 senza
  // batteria realmente scarica) → falso positivo. Lo sopprimiamo quando il bypass
  // e' attivo. Rischio accettato: una batteria che si scaricasse *durante* un
  // bypass verrebbe segnalata solo a bypass tolto (condizione lenta, ricompare).
  bool battery_low = ((b2 & 0x40) == 0) && !bypass;

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

  // Applicazione IMMEDIATA per entrambi i parser (nessun debounce). Il debounce
  // sull'allarme e' stato tolto: durante un allarme e' il DISPLAY (frame 0x04) a
  // essere fitto, mentre i frame 0x0C -- gli unici su cui gira process_armed --
  // restano RADI (uno per ciclo). Un debounce a 2 frame 0x0C ritarderebbe (o
  // mancherebbe) lo scatto, inaccettabile per un allarme. Il rischio-fantasma del
  // nuovo framer (collisione marcatore-spazzatura) e' bassissimo, mai emerso nei
  // test, e auto-correttivo al frame successivo: preferibile a un allarme lento.
  SystemState target = (alarm && armed)         ? STATE_ALARM
                     : (armed && partial_seen_) ? STATE_ARMED_PARTIAL
                     : armed                    ? STATE_ARMED_TOTAL
                     :                            STATE_DISARMED;
  set_system_state(target);
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
  if (deterministic_tx_) {
    // Percorso deterministico: accoda i tasti nella FreeRTOS queue (thread-safe
    // e cross-core), tx_task ne estrae uno per impulso di sync. Niente guardia
    // "TX in corso": la coda gestisce la concorrenza.
    int n = 0;
    for (int i = 0; keys[i]; i++) {
      uint8_t k = char_to_key(keys[i]);
      if (k == 0xFF) continue;
      if (xQueueSend(key_queue_, &k, 0) == pdTRUE) n++;
      else ESP_LOGW(TAG, "coda TX piena, tasto perso");
    }
    if (n) ESP_LOGI(TAG, "Coda: %d tasti", n);
    return;
  }
  // --- percorso vecchio (deterministic_tx: false) ---
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
