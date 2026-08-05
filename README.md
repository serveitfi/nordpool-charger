# ESP32 Nord Pool Smart Charger Controller

An ESP32-based smart charger controller that uses real-time Nord Pool electricity spot prices to control a dry contact relay. Charges your EV (or any device) when electricity is cheap and stops when it's expensive.

![Firmware](https://img.shields.io/badge/firmware-v0.93-brightgreen) ![Platform](https://img.shields.io/badge/platform-ESP32-blue) ![License](https://img.shields.io/badge/license-MIT-orange)

## Features

**Core functionality**
- Real-time electricity prices from the free Nord Pool dataportal API (no API key needed)
- Dry contact relay control based on configurable price threshold
- Today + tomorrow prices (next-day prices fetched automatically after 14:00)
- 15-minute price resolution for Finland, hourly for other areas
- Fail-safe design: power loss → relay de-energizes → NC closes → charging stays ON

**User interface**
- Dark-themed responsive web dashboard with interactive price chart
- Tap/hover tooltips showing price, time, and charging status per bar
- Color-coded bars: green (charging), red (stopped), blue (forced ON), white (current period)
- Quick controls on dashboard for max price and always-allowed hours
- Manual override button: force charging ON for 30min – 12 hours
- Full settings page with WiFi, timezone, area, currency, VAT, tariff configuration

**Smart features**
- Always-allowed charging hours (e.g., 02:00–07:00 overnight, overrides price)
- Electricity company tariff/margin added on top of spot price
- Automatic VAT per country (Finland 25.5%, Sweden 25%, etc.)
- Configurable timezone with auto-detection from price area
- Custom mDNS hostname (e.g., `http://mycharger.local`)
- WiFi auto-reconnect with AP fallback after 2 minutes
- OTA firmware updates via web browser (no USB needed after first flash)
- REST API endpoint (`/api/prices`) for Home Assistant or other integrations

**Stability**
- Zero heap-allocation page rendering (PROGMEM + chunked HTTP)
- JSON deserialization filter (parses only selected area, ~90% memory savings)
- Price data preserved on fetch failure (restores previous data)
- Fetch-in-progress detection (serves lightweight "loading" page during HTTPS)
- Consistent page rendering via global state snapshots (no race conditions)

## Hardware

### Components

| Component | Example | Notes |
|-----------|---------|-------|
| ESP32 Dev Board | ESP32-WROOM-32 DevKit | Any ESP32 with WiFi |
| Relay Module | Songle SRD-05VDC-SL-C | 5V module with built-in driver |
| Jumper Wires | 3 pcs | ESP32 → relay module |
| Wire | 2 pcs | Relay screw terminals → charger dry contact |

No external transistors, resistors, or diodes needed when using a relay module.

### Wiring — Relay Module (Recommended)

```
    Relay Module              ESP32
   ┌───────────┐
   │  IN ──────────────────── GPIO 26
   │  GND ─────────────────── GND
   │  VCC ─────────────────── 5V (or VIN)
   │           │
   │  COM ─────┤──── wire ──── Charger Enable +
   │  NC  ─────┤──── wire ──── Charger Enable -
   │  NO  (not used)
   └───────────┘
```

**Important:** Wire between **COM** and **NC** (Normally Closed) for fail-safe operation.

### Why NC (Normally Closed)?

| ESP32 State | Relay | NC Contact | Charging |
|-------------|-------|------------|----------|
| Power lost | De-energized | **Closed** | **ON** ✓ |
| Running, cheap price | De-energized | **Closed** | **ON** ✓ |
| Running, expensive | Energized | **Open** | **OFF** ✗ |

### Wiring — Bare Transistor (Alternative)

For direct relay control without a module:

```
                    +5V
                      │
                 ┌────┴────┐
                 │  RELAY   │
                 │   COIL   │
                 └────┬────┘
             ┌────┐   │
    Diode    │ ◄──┼───┤      COM ─── To Charger
  (1N4007)   │    │   │      NC  ─── To Charger
             └────┼───┘
                  │ (Collector)
              ┌───┴───┐
              │  NPN  │  (2N2222 / BC547)
              └───┬───┘
                  │ (Base)
    GPIO 26 ──[1kΩ]──┘
                  │ (Emitter)
                 GND
```

### Active LOW Relay Modules

Most relay modules are active LOW (relay clicks when IN = LOW). If your relay behaves opposite to expected, swap `HIGH`/`LOW` in the `digitalWrite(RELAY_PIN, ...)` calls in `updateRelay()`.

## Getting Started

### Prerequisites

1. **Arduino IDE 2.x** or **PlatformIO**
2. **ESP32 Board Package** — add `https://dl.espressif.com/dl/package_esp32_index.json` to Board Manager URLs
3. **ArduinoJson** library v7+ — install via Library Manager

### First Flash

1. Open `nordpool_charger.ino` in Arduino IDE
2. Select board: **ESP32 Dev Module**
3. Upload to ESP32 via USB
4. Connect to WiFi hotspot **NordPool-Charger** (open network)
5. Captive portal opens → select your WiFi, set price area
6. ESP32 reboots → visit **http://charger.local**

### Subsequent Updates (OTA)

1. Arduino IDE: **Sketch → Export Compiled Binary**
2. Open **Settings** page on the charger
3. Scroll to **Firmware Update**, select the `.bin` file
4. Click **Upload & Update** — device reboots with new firmware
5. All settings are preserved (stored in flash, separate from firmware)

## Web Interface

### Dashboard

The main page shows:
- **Relay status** — CHARGING ON / CHARGING OFF / OVERRIDE ON / LOADING PRICES
- **Current price** with today's min and max
- **Price chart** — interactive bar chart with today + tomorrow
- **Manual override** — force charging ON for a selected duration
- **Quick controls** — max price and always-allowed hours

The page auto-refreshes every 60 seconds. During price fetches, a lightweight "Fetching prices..." page is shown to avoid slow responses.

### Settings

- **WiFi** — change SSID/password (reboots to apply)
- **Device Name** — custom mDNS hostname
- **Price Area** — delivery area with automatic VAT rate
- **Timezone** — auto-set from area, manual override available
- **Currency** — EUR, SEK, NOK, DKK
- **VAT** — toggle on/off, rate auto-detected per country
- **Tariff** — electricity company margin added to spot price
- **Price Threshold** — max price for charging
- **Always Allowed Hours** — time window that overrides price threshold
- **Firmware Update** — upload `.bin` via browser
- **Reboot / Factory Reset**

Version number and build date shown at top of settings page.

## Charging Logic

Priority order (highest to lowest):

| Priority | Condition | Result |
|----------|-----------|--------|
| 1 | Manual override active | Charging ON |
| 2 | Current time in always-allowed window | Charging ON |
| 3 | Current price ≤ max price threshold | Charging ON |
| 3 | Current price > max price threshold | Charging OFF |
| 4 | No threshold set | Charging ON |

### Examples

| Max Price | Always Allowed | Override | Behavior |
|-----------|---------------|----------|----------|
| Not set | Off | Off | Always charges |
| 8.0 c/kWh | Off | Off | Charges when price ≤ 8.0 |
| 8.0 c/kWh | 02:00–07:00 | Off | Price-based, but always charges 02–07 |
| 8.0 c/kWh | 02:00–07:00 | 2h | Charges next 2 hours regardless |

## Next-Day Prices

Nord Pool publishes next-day prices between 14:00–14:45 CET (~15:00–15:45 Finnish time).

The device:
- Checks every **5 minutes** between 14:00–16:00 local time
- Once found, tomorrow's prices appear on the chart with dimmer colors
- Dashed separator line marks the boundary between today and tomorrow
- Switches to **15-minute** intervals after tomorrow is loaded
- Resets at midnight for the new day

## Price Calculation

```
Display price = (API price in EUR/MWh ÷ 10) × (1 + VAT%) + tariff

Example (Finland):
  50.00 EUR/MWh ÷ 10 × 1.255 + 0.41 = 6.685 c/kWh
```

### VAT Rates

| Country | Rate | Local Name |
|---------|------|------------|
| **Finland** | **25.5%** | ALV |
| Sweden, Norway, Denmark | 25% | Moms / MVA |
| Estonia | 22% | KM |
| Latvia, Lithuania | 21% | PVN / PVM |
| Austria, France | 20% | MwSt / TVA |
| Germany | 19% | MwSt |
| Belgium, Netherlands | 21% | BTW |
| Poland | 23% | VAT |

## Supported Areas

Finland (FI), Sweden (SE1–SE4), Norway (NO1–NO5), Denmark (DK1–DK2), Estonia (EE), Latvia (LV), Lithuania (LT), Austria (AT), Belgium (BE), Germany (DE-LU), France (FR), Netherlands (NL), Poland (PL)

## Timezones

| Timezone | Areas | UTC Offset |
|----------|-------|------------|
| EET/EEST | Finland, Estonia, Latvia, Lithuania | UTC+2 / +3 |
| CET/CEST | Sweden, Norway, Denmark, Germany, France, etc. | UTC+1 / +2 |
| GMT/BST | UK, Ireland, Portugal | UTC+0 / +1 |

Timezone auto-updates when you change the price area. Can be overridden manually in Settings.

## REST API

`GET /api/prices`

```json
{
  "version": "0.93",
  "build": "Mar 20 2026 12:55:17",
  "area": "FI",
  "currency": "EUR",
  "timezone": "EET-2EEST,M3.5.0/3,M10.5.0/4",
  "vatEnabled": true,
  "vatRate": 25.5,
  "tariff": 0.41,
  "currentPrice": 3.67,
  "unit": "c/kWh",
  "relayOpen": false,
  "charging": true,
  "priceStop": 8.0,
  "forceChargeStart": 2,
  "forceChargeEnd": 7,
  "overrideMinRemaining": 0,
  "priceCount": 192,
  "todayCount": 96,
  "tomorrowLoaded": true,
  "minPrice": 1.47,
  "maxPrice": 6.87,
  "freeHeap": 225644,
  "prices": [
    {"time": "00:00", "price": 3.93, "tomorrow": false},
    {"time": "00:15", "price": 3.65, "tomorrow": false},
    ...
    {"time": "23:45", "price": 0.19, "tomorrow": true}
  ]
}
```

## WiFi Behavior

| State | Behavior |
|-------|----------|
| First boot (no WiFi saved) | Opens AP hotspot for configuration |
| WiFi connected | Normal operation |
| WiFi drops | Reconnects every 30 seconds |
| WiFi disconnected > 2 min | Falls back to AP hotspot mode |
| WiFi/mDNS changed in settings | Reboots to apply |

## Troubleshooting

**Prices not loading** — Check serial monitor. Common causes: WiFi disconnected, NTP not synced (year shows 1970), Nord Pool API temporarily unavailable. The device retries every 60 seconds.

**"Fetching prices..." page on every load** — A price fetch is in progress (takes 5–15 seconds due to TLS). Wait for it to complete and refresh.

**Chart shows only a few wide bars** — NTP hadn't synced when prices were fetched (wrong date). Reboot the device or wait for the next fetch cycle.

**Min/Max shows "--"** — Prices haven't loaded yet. Wait for the first successful fetch.

**Relay behaves backwards** — Your module may be active LOW. Swap `HIGH`/`LOW` in `updateRelay()`.

**Can't access charger.local** — mDNS doesn't work on all networks/devices. Use the IP address from serial monitor or check your router's DHCP client list.

**Tomorrow's prices not appearing** — Published around 14:00–14:45 CET. The device checks every 5 minutes after 14:00 local time. Verify via `/api/prices` → `tomorrowLoaded` field.

**OTA update fails** — Ensure the `.bin` file is an ESP32 firmware binary (Sketch → Export Compiled Binary), not the `.ino` source file. A failed update boots back to the previous firmware.

## Technical Notes

### Memory Management

The firmware is designed for weeks of continuous operation:

- Page rendering uses `server.sendContent_P(PSTR(...))` — HTML/CSS/JS from flash, zero heap allocation
- Price data sent entry-by-entry via chunked HTTP (80-byte stack buffer per entry)
- JSON parsing uses ArduinoJson filter to parse only the selected delivery area (~90% memory reduction)
- API responses read via `getString()` to handle chunked transfer encoding
- No String concatenation in hot paths — all dynamic content uses `snprintf()` with stack buffers
- Global state snapshots prevent race conditions during page rendering
- Heap usage logged every 5 minutes to serial monitor

### Nord Pool API

Uses the free, public dataportal API:
```
GET https://dataportal-api.nordpoolgroup.com/api/DayAheadPrices
    ?date=2026-03-20&market=DayAhead&deliveryArea=FI&currency=EUR
```

- No API key or authentication required
- Returns EUR/MWh without VAT
- Finland uses 15-minute delivery intervals
- HTTPS with certificate verification skipped (`setInsecure()`) for ESP32 compatibility

### Pin Configuration

| Pin | Function |
|-----|----------|
| GPIO 26 | Relay control (configurable via `RELAY_PIN`) |
| GPIO 2 | Built-in LED (status indicator) |

## Configuration

Edit these `#define` values before compiling:

```c
#define FW_VERSION           "0.93"
#define RELAY_PIN            26
#define NTP_SERVER           "pool.ntp.org"
#define TIMEZONE             "EET-2EEST,M3.5.0/3,M10.5.0/4"  // Default
#define MDNS_NAME            "charger"
#define AP_SSID              "NordPool-Charger"
#define PRICE_FETCH_INTERVAL 900000  // 15 minutes in ms
```

All runtime settings (WiFi, area, timezone, thresholds, tariff) are stored in ESP32 flash and persist across reboots and firmware updates.

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| [ArduinoJson](https://arduinojson.org/) | 7.x | JSON parsing with filter support |
| ESP32 Arduino Core | 2.x+ | WiFi, WebServer, Preferences, Update, mDNS |

## License

MIT
