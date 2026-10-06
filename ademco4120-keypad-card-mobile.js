console.info(
  "%c  ADEMCO-4120-CARD-MOBILE %c v2.0 ",
  "color: white; font-weight: bold; background: #059669",
  "color: white; font-weight: bold; background: #d32f2f"
);

const ZONES_TTL = 120000;   // scansione zone a vista per 2 minuti
const BUFFER_TTL = 30000;   // tasti digitati e non inviati: azzerati dopo 30 s
const NO_ZONE = "nessuna";  // valore del firmware per "nessuna zona"

const LCD_STATE = {
  disarmed: "DISINSERITO",
  armed_total: "INSERITO TOTALE",
  armed_partial: "INSERITO PARZIALE",
  alarm: "⚠ ALLARME ATTIVO ⚠",
  unknown: "---",
};

// Icone di stato sotto l'LCD: [id, chiave config, icona on, icona off, colore]
const STATUS_ICONS = [
  ["icon-armed",   "sensor_armed",   "mdi:shield-lock",      "mdi:shield-off-outline",      "red"],
  ["icon-total",   "sensor_total",   "mdi:shield-lock",      "mdi:shield-outline",          "red"],
  ["icon-partial", "sensor_partial", "mdi:shield-half-full", "mdi:shield-outline",          "orange"],
  ["icon-alarm",   "sensor_alarm",   "mdi:alarm-light",      "mdi:alarm-light-off-outline", "red"],
];

// Tastiera: colonne di [tasto, etichetta comando]
const KEYPAD = [
  [["1", "OFF"], ["4", "MAX PROT"], ["7", "IMMEDIATO"], ["*", "PRONTO"]],
  [["2", "STAY"], ["5", "PROVA"], ["8", "CODICI"], ["0", ""]],
  [["3", "AWAY"], ["6", "ESCLUSO"], ["9", "CHIME"], ["#", null]],
];

const STYLE = `
  :host { display: block; }
  ha-card { padding-bottom: 16px; }
  .wrap { display: flex; flex-direction: column; align-items: center; width: 100%; }
  .keypad { width: 100%; max-width: 400px; }

  .lcd {
    display: flex; flex-direction: column; align-items: center; gap: 6px;
    background: var(--lcdbg, #859c99);
    border: 1px solid var(--bordercolor, #ccc);
    border-radius: 10px;
    margin: 8px auto; padding: 8px 12px;
    max-width: 95%; min-height: 40px; box-sizing: border-box;
  }
  .lcd-line {
    display: flex; justify-content: center; align-items: center;
    width: 100%; min-height: 1.4em;
    font: bold 1.2rem Arial, sans-serif;
    color: var(--lcdtext, #222);
    text-align: center;
  }
  .lcd-line.sending { opacity: .4; animation: blink .7s step-start infinite; }
  @keyframes blink { 50% { opacity: .1; } }

  .zones {
    display: flex; justify-content: center; align-items: center;
    max-width: 90%; min-height: 1.4em; margin: 2px 0; padding: 4px 10px;
    font: bold .9rem Roboto, sans-serif; letter-spacing: .03em;
    color: var(--lcdtext, #222);
    box-sizing: border-box;
  }
  .zones.hidden { visibility: hidden; }

  .badges { display: flex; flex-direction: column; gap: 4px; width: 100%; margin-top: 4px; }
  .badges-row { display: flex; gap: 6px; width: 100%; }
  .badge {
    flex: 1; padding: 2px 4px; border-radius: 6px;
    font: bold .65rem Arial, sans-serif; text-align: center; white-space: nowrap;
    background: rgba(0,0,0,.15); color: #555; opacity: .4;
  }
  .badge.green  { background: #1a7a1a; color: #aeffae; opacity: 1; }
  .badge.red    { background: #7a1a1a; color: #ffaeae; opacity: 1; }
  .badge.orange { background: #7a5a00; color: #ffe090; opacity: 1; }
  .badge.yellow { background: #8a8a00; color: #ffffa0; opacity: 1; }

  .icons { display: flex; width: 100%; margin: 4px 0; }
  .icon-btn {
    display: flex; flex-direction: column; align-items: center; flex: 1; padding: 4px;
    color: var(--sensoroff, #aaa);
  }
  .icon-btn.active { filter: drop-shadow(0 0 3px currentColor); }
  .icon-label { font-size: .5rem; letter-spacing: 1px; color: var(--sensorlabel, var(--accent-color)); }

  .pad { display: flex; justify-content: center; width: 100%; }
  .col { display: flex; flex-direction: column; }
  button {
    display: flex; flex-direction: column; align-items: center; justify-content: center;
    min-width: 68px; height: 44px; margin: 4px; padding: 2px;
    border: 1px solid var(--bordercolor, #ccc); border-radius: 4px;
    background: var(--buttonbg, var(--input-fill-color));
    color: var(--buttontext, var(--primary-color));
    font: bold .85rem Roboto, sans-serif;
    cursor: pointer; user-select: none; -webkit-tap-highlight-color: transparent;
    box-sizing: border-box;
  }
  button:active { background: var(--buttonactive, #555) !important; }
  button:disabled { opacity: .4; cursor: not-allowed; }
  .kcmd { width: 100%; margin-top: 2px; font-size: .65rem; line-height: 1; opacity: .75; white-space: nowrap; }
  button.enter.has-keys { color: #2a9d2a; border-color: #2a9d2a; }
`;

class Ademco4120CardMobile extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._keyBuffer = [];
    this._sending = false;
    this._bufferTimer = null;
    this._zonesTimer = null;       // scansione zone a vista
    this._lastZones = null;        // ultimo valore visto di sensor_zones
    this._bypassShown = false;     // la riga zone sta mostrando il bypass
  }

  setConfig(config) {
    if (!config) throw new Error("Configurazione non valida");
    this._config = {
      max_keys: 11,
      vibration_duration: 5,
      // sensori LED batteria/rete: configurabili, default = entita' dell'impianto
      sensor_battery: "binary_sensor.allarme_ademco_4120_batteria_scarica",
      sensor_mains: "binary_sensor.allarme_ademco_4120_rete_220v",
      ...config,
    };
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
    this._updateState();
  }

  disconnectedCallback() {
    clearTimeout(this._bufferTimer);
    clearTimeout(this._zonesTimer);
  }

  getCardSize() { return 6; }

  // ── helpers ──────────────────────────────────────────────────────────
  $(id) { return this.shadowRoot.getElementById(id); }

  _isOn(entityId) {
    const s = entityId && this._hass?.states[entityId];
    return !!s && (s.state === "on" || s.state === "true");
  }

  _stateOf(entityId) {
    const s = entityId && this._hass?.states[entityId];
    if (!s || s.state === "unknown" || s.state === "unavailable") return null;
    return s.state;
  }

  _vibrate() {
    if ("vibrate" in navigator) navigator.vibrate(this._config.vibration_duration);
  }

  _press(entityId) {
    if (this._hass && entityId) this._hass.callService("button", "press", { entity_id: entityId });
  }

  _setBadge(id, color) {
    const el = this.$(id);
    if (el) el.className = "badge" + (color ? ` ${color}` : "");
  }

  _setKeysEnabled(enabled) {
    this.shadowRoot.querySelectorAll("button[data-key], button.enter").forEach(b => (b.disabled = !enabled));
  }

  // ── render ───────────────────────────────────────────────────────────
  _render() {
    const c = this._config;
    const keyButton = ([key, label]) =>
      `<button data-key="${key}">${key}${label === null ? "" : `<div class="kcmd">${label || "&nbsp;"}</div>`}</button>`;
    const columns = KEYPAD.map(col => `<div class="col">${col.map(keyButton).join("")}</div>`).join("");

    this.shadowRoot.innerHTML = `
      <style>${STYLE}${c.scale ? `.wrap { zoom: ${c.scale}; }` : ""}</style>
      <ha-card>
        <div class="wrap"><div class="keypad">
          <div class="lcd">
            <div class="lcd-line" id="lcd">---</div>
            <div class="zones hidden" id="zones"></div>
            <div class="badges">
              <div class="badges-row">
                <span class="badge" id="badge-ready">PRONTO</span>
                <span class="badge" id="badge-batt">BATT</span>
                <span class="badge" id="badge-mains">RETE</span>
              </div>
              <div class="badges-row">
                <span class="badge" id="badge-notready">NON PRONTO</span>
                <span class="badge" id="badge-chime">CHIME</span>
                <span class="badge" id="badge-bypass">BYPASS</span>
              </div>
            </div>
          </div>

          <div class="icons">
            ${STATUS_ICONS.map(([id, , , off]) => {
              const label = id.replace("icon-", "").replace("armed", "inserito").replace("total", "totale")
                .replace("partial", "parziale").replace("alarm", "allarme").toUpperCase();
              return `<div class="icon-btn" id="${id}"><ha-icon icon="${off}"></ha-icon><span class="icon-label">${label}</span></div>`;
            }).join("")}
          </div>

          <div class="pad">
            ${columns}
            <div class="col">
              <button data-action="A">${c.button_A || "OFF"}</button>
              <button data-action="B">${c.button_B || "TOTALE"}</button>
              <button data-action="C">${c.button_C || "PARZ."}</button>
              <button class="enter" id="btn-enter">INVIO</button>
            </div>
          </div>
        </div></div>
      </ha-card>`;

    // variabili CSS passate da YAML (style: { --lcdbg: ..., ... })
    for (const [k, v] of Object.entries(c.style || {})) {
      if (v) this.style.setProperty(k, String(v).replace(/;/g, ""));
    }

    this.shadowRoot.querySelectorAll("button[data-key]").forEach(btn =>
      btn.addEventListener("click", () => this._onKey(btn.dataset.key)));
    this.shadowRoot.querySelectorAll("button[data-action]").forEach(btn =>
      btn.addEventListener("click", () => this._onAction(btn.dataset.action)));
    this.$("btn-enter").addEventListener("click", () => this._onEnter());

    this._updateState();
  }

  // ── input ────────────────────────────────────────────────────────────
  _onKey(key) {
    if (key === "*") {                       // * = scansione zone aperte
      if (!this._config.entity_scan_zones) return;
      this._vibrate();
      this._press(this._config.entity_scan_zones);
      this._showZones(this._stateOf(this._config.sensor_zones));
      return;
    }
    if (this._sending || this._keyBuffer.length >= this._config.max_keys) return;
    this._vibrate();
    this._keyBuffer.push(key);
    this._updateBuffer();
  }

  _onAction(action) {
    this._vibrate();
    this._press(this._config[`entity_${action}`]);
  }

  _onEnter() {
    if (this._sending || this._keyBuffer.length === 0) return;
    this._vibrate();
    this._sendSequence();
  }

  _updateBuffer() {
    clearTimeout(this._bufferTimer);
    const lcd = this.$("lcd");
    if (this._keyBuffer.length > 0) {
      lcd.textContent = "● ".repeat(this._keyBuffer.length).trim();
      this._bufferTimer = setTimeout(() => { this._keyBuffer = []; this._updateBuffer(); }, BUFFER_TTL);
    } else {
      this._updateState();
    }
    this.$("btn-enter").classList.toggle("has-keys", this._keyBuffer.length > 0);
  }

  async _sendSequence() {
    if (!this._hass || !this._config.entity_send_keys) return;
    clearTimeout(this._bufferTimer);
    this._sending = true;
    this._setKeysEnabled(false);
    this.$("lcd").classList.add("sending");

    // Il firmware svuota il campo dopo ogni invio, quindi anche una sequenza
    // identica alla precedente e' un cambio di valore reale e scatena l'azione.
    await this._hass.callService("text", "set_value", {
      entity_id: this._config.entity_send_keys,
      value: this._keyBuffer.join(""),
    });

    this._keyBuffer = [];
    this._sending = false;
    this.$("lcd").classList.remove("sending");
    this.$("btn-enter").classList.remove("has-keys");
    this._setKeysEnabled(true);
    this._updateState();
  }

  // ── stato ────────────────────────────────────────────────────────────
  _updateState() {
    if (!this._hass || !this._config || !this.shadowRoot.innerHTML) return;
    const c = this._config;

    if (this._keyBuffer.length === 0 && !this._sending) {
      const st = this._hass.states[c.sensor_state];
      this.$("lcd").textContent = st ? (LCD_STATE[st.state] || st.state.toUpperCase()) : "---";
    }

    // Da inserito, PRONTO / NON PRONTO non hanno senso (sul pannello fisico
    // l'indicatore sparisce): entrambi spenti.
    const armed = this._isOn(c.sensor_armed);
    const ready = this._isOn(c.sensor_ready);
    this._setBadge("badge-ready",    !armed && ready  ? "green" : "");
    this._setBadge("badge-notready", !armed && !ready ? "red" : "");
    this._setBadge("badge-chime",  this._isOn(c.sensor_chime)   ? "yellow" : "");
    this._setBadge("badge-bypass", this._isOn(c.sensor_bypass)  ? "orange" : "");
    this._setBadge("badge-batt",   this._isOn(c.sensor_battery) ? "red" : "");
    this._setBadge("badge-mains",  this._isOn(c.sensor_mains)   ? "green" : "");

    for (const [id, key, onIcon, offIcon, color] of STATUS_ICONS) {
      const el = this.$(id);
      const active = this._isOn(c[key]);
      el.querySelector("ha-icon").setAttribute("icon", active ? onIcon : offIcon);
      el.style.color = active ? color : "";
      el.classList.toggle("active", active);
    }

    this._updateZones();
  }

  // ── riga zone: allarme > bypass > scansione (con TTL) ────────────────
  _setZonesText(text, visible) {
    const el = this.$("zones");
    el.textContent = text ?? "";
    el.classList.toggle("hidden", !visible);
  }

  _showZones(text) {
    if (text === null || text === NO_ZONE) return;
    this._setZonesText(text, true);
    clearTimeout(this._zonesTimer);
    this._zonesTimer = setTimeout(() => { this._zonesTimer = null; this._setZonesText("", false); }, ZONES_TTL);
  }

  _updateZones() {
    const c = this._config;

    // 1) allarme in corso: zona che ha fatto scattare, sempre a vista
    if (this._isOn(c.sensor_alarm) && c.sensor_alarm_zone) {
      const raw = this._hass.states[c.sensor_alarm_zone]?.state;
      if (raw) { this._setZonesText(this._stateOf(c.sensor_alarm_zone), true); return; }
    }

    // 2) zona con bypass modificato: a vista finche' il firmware la tiene,
    //    sparisce subito quando torna "nessuna" (senza toccare il timer scansione)
    if (c.sensor_zone_changed) {
      const zone = this._stateOf(c.sensor_zone_changed);
      if (zone && zone !== NO_ZONE) {
        this._bypassShown = true;
        this._setZonesText(zone, true);
        return;
      }
      if (this._bypassShown) {
        this._bypassShown = false;
        this._setZonesText("", false);
        return;
      }
    }

    // 3) zone aperte: a vista per ZONES_TTL quando cambiano o dopo la scansione (*)
    if (!c.sensor_zones || !this._hass.states[c.sensor_zones]) return;
    const zones = this._hass.states[c.sensor_zones].state || NO_ZONE;

    if (this._lastZones === null) {           // primo aggiornamento: solo memorizza
      this._lastZones = zones;
      this._setZonesText("", false);
      return;
    }

    if (zones === NO_ZONE) {
      this._lastZones = zones;
      clearTimeout(this._zonesTimer);
      this._zonesTimer = null;
      this._setZonesText("", false);
      return;
    }

    if (this._lastZones !== zones || this._zonesTimer) {
      this._lastZones = zones;
      const text = this._stateOf(c.sensor_zones) ?? "";
      if (this._zonesTimer) this._setZonesText(text, true);
      else this._showZones(text);
    }
  }
}

customElements.define("ademco4120-keypad-card-mobile", Ademco4120CardMobile);

window.customCards = window.customCards || [];
window.customCards.push({
  type: "ademco4120-keypad-card-mobile",
  name: "Ademco 4120 Keypad Card Mobile",
  description: "Tastiera con Zone + Allarme Zona",
  preview: true,
});
