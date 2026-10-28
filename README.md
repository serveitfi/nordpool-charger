# NordPool Charger

ESP32 firmware that turns EV charging (or any load with an enable input) on and off based on
Nord Pool day-ahead electricity prices. It drives a relay as a dry contact, so it works with any
charger or device that has a "charging allowed" / external enable input.

Everything is configured from a built-in web page. No cloud service, app or account is needed.

## Features

- **Live price chart** for today and tomorrow (15-minute or hourly slots, depending on the area)
- **Max price**: charge only when the price is at or below a limit
- **Cheapest hours**: charge only during the N cheapest hours within a time window,
  e.g. "4 cheapest hours between 22:00 and 07:00"
- **Roof price**: never charge above this price, whatever the other rules say
- **Always allowed hours**: a daily time window when charging is always on
- **Manual override**: force charging on for 0.5–12 hours with one button
- **Fail-safe**: if the ESP32 loses power, or prices can't be fetched, charging stays **on**
- **VAT and tariff**: prices include VAT for the selected area and an optional fixed
  margin/transfer fee
- **21 price areas**: FI, SE1–4, DK1–2, NO1–5, EE, LV, LT, AT, BE, DE-LU, FR, NL, PL
- **WiFi setup portal**: on first boot the device opens its own access point for setup
- **mDNS**: open `http://charger.local` (configurable name)
- **Firmware update over WiFi** from the settings page
- **JSON API** at `/api/prices` for Home Assistant, scripts, etc.

## Hardware

- ESP32 or ESP32-S3 dev board (developed on an ESP32-S3-WROOM-1 with 8 MB flash)
- Relay module with a **normally closed (NC)** contact, or a bare relay driven by a transistor:

```
GPIO 14 ──[1 kΩ]──┤ B   NPN (e.g. 2N2222)
                  │ C ──── relay coil ──── VCC (relay voltage)
                  │ E ──── GND             (flyback diode across the coil)

Relay COM + NC  ──>  charger's enable / "charging allowed" input
```

| GPIO 14 | Relay        | NC contact | Charging |
|---------|--------------|------------|----------|
| LOW     | not energized| closed     | **ON**   |
| HIGH    | energized    | open       | OFF      |
| no power| not energized| closed     | **ON**   |

Wiring the NC contact means a dead or rebooting ESP32 never blocks charging.

The relay pin is set with `RELAY_PIN` near the top of the sketch. The status LED uses `LED_PIN`
(GPIO 2): it blinks fast in setup mode and stays on while running. Many ESP32-S3 boards have an
RGB LED instead, so the LED may not light on those. This doesn't affect anything else.

> ⚠️ Only switch the charger's low-voltage control input with this relay, never mains power to the
> charger. Check your charger's manual for how its external enable input works.

## Building

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) and the **esp32** board package by
   Espressif (Boards Manager). Tested with esp32 core 3.3.x.
2. Install the **ArduinoJson** library by Benoit Blanchon (Library Manager, version 7.x).
3. Open `nordpool_charger/nordpool_charger.ino`.
4. Select your board under **Tools**, for example for an ESP32-S3 with 8 MB flash:

   | Setting          | Value                                   |
   |------------------|-----------------------------------------|
   | Board            | ESP32S3 Dev Module                      |
   | Flash Size       | 8MB (64Mb)                              |
   | Partition Scheme | 8M with spiffs (3MB APP/1.5MB SPIFFS)   |

   For a classic ESP32, pick **ESP32 Dev Module** and a partition scheme with OTA.
   The firmware is about 1.2 MB, so it fits the default 4 MB layout.
5. Upload over USB the first time.

A binary built for one chip doesn't run on another: an ESP32 build won't work on an ESP32-S3.
A change of **Partition Scheme** always needs a USB upload. Firmware updates over WiFi only
replace the firmware, not the flash layout.

## First setup

1. Power on the device. It opens a WiFi network called **`NordPool-Charger`** (no password).
2. Connect to it with your phone. The setup page should open by itself; if not, go to
   `http://192.168.4.1`.
3. Pick your WiFi network, enter the password, choose your price area and press
   **Save & Connect**.
4. The device reboots and joins your WiFi. Open **`http://charger.local`**, or use the IP address
   shown by your router.

If the device loses WiFi for more than 2 minutes, it opens the setup network again, so you can
reconfigure it.

## Using it

The dashboard shows the relay status, the current price, today's min/max and the price chart.
Green bars are slots where it charges, red where it doesn't, blue the always-allowed hours and dark
red the slots above the roof price. Tap or hover a bar for details.

### Charging rules

From highest to lowest priority:

1. **Manual override**: charges for the selected time, even above the roof price.
2. **No prices loaded**: charges (fail-safe).
3. **Roof price**: never charges above it.
4. **Always allowed hours**: charges during this daily window.
5. **Cheapest hours** (when set): charges only in the selected cheapest slots of the window.
   If a max price is also set, it also charges whenever the price is at or below it.
6. **Max price** (when set): charges when the price is at or below it.
7. **Nothing set**: always charges.

All prices are compared in c/kWh, after VAT (if enabled) and tariff are added.

### Cheapest hours

Enter the number of hours you need, e.g. `4` or `2.75`, and a window, e.g. `22 – 07`. The device
picks the cheapest slots inside the window. They don't have to be consecutive. Leave the hours
empty to turn the mode off. If start and end are the same hour, the window is 24 hours.

- To estimate the hours, divide the energy needed (kWh) by the charging power (kW), and add a
  little extra.
- Nord Pool publishes next-day prices in the early afternoon, and the device starts checking for
  them at 14:00 local time. Start the window after about 15:00, so the whole window is priced when
  the plan is made. Until then the dashboard shows "waiting for next-day prices".
- With a roof price, slots above the roof are skipped. If too few slots are left, you get fewer
  hours, and the dashboard shows e.g. "only 3h below roof".
- After a reboot in the middle of a window, the device plans the full hours again from that moment
  on, so it may charge a bit more than requested, never less.

### Price updates

Prices are fetched every 15 minutes, every 5 minutes between 14:00 and 16:00 until tomorrow's
prices appear, and every 60 seconds if no prices are loaded yet.

## Settings page

`http://charger.local/settings` contains:

- WiFi network and device name (mDNS). Changing either reboots the device.
- Price area, currency and VAT on/off
- Timezone (set automatically from the price area)
- Tariff / margin in c/kWh, added on top of the spot price
- Max price, roof price, always allowed hours and cheapest hours
- Firmware update, reboot and factory reset

All settings are stored in flash and survive reboots and firmware updates.

## Firmware update over WiFi

1. In the Arduino IDE: **Sketch → Export Compiled Binary**, using the same Tools settings as the
   USB upload.
2. Open **Settings → Firmware Update**, choose the `.bin` file from the sketch's `build/` folder
   and upload.
3. The device reboots. The settings page shows the new firmware version.

## JSON API

`GET /api/prices` returns the current state and all loaded prices:

```json
{
  "version": "0.96",
  "area": "FI",
  "currency": "EUR",
  "vatEnabled": true,
  "vatRate": 25.5,
  "tariff": 0.50,
  "currentPrice": 4.12,
  "unit": "c/kWh",
  "relayOpen": false,
  "charging": true,
  "priceStop": -1.0,
  "forceChargeStart": -1,
  "forceChargeEnd": -1,
  "overrideMinRemaining": 0,
  "priceRoof": 15.0,
  "cheapHours": 4.00,
  "cheapStart": 22,
  "cheapEnd": 7,
  "cheapSlots": 16,
  "cheapPlannedHours": 4.00,
  "cheapPlanComplete": true,
  "cheapAvg": 2.85,
  "priceCount": 192,
  "todayCount": 96,
  "tomorrowLoaded": true,
  "minPrice": 1.90,
  "maxPrice": 12.40,
  "freeHeap": 180000,
  "prices": [
    { "time": "00:00", "price": 3.95, "tomorrow": false, "cheap": false }
  ]
}
```

The response also includes `build` and `timezone`. A value of `-1` means "not set".
`relayOpen: true` means charging is blocked.

## Security notes

- The web interface has **no password**. Anyone on the same network can change settings or
  upload firmware. Use it only on a trusted home network, and don't expose it to the internet.
- The setup access point is open (no password) while the device is in setup mode.
- Price data is fetched over HTTPS, but the server certificate isn't verified.

## Data source

Prices come from the public Nord Pool data portal API
(`dataportal-api.nordpoolgroup.com`). It's not an official or guaranteed API, so it may change or
be rate-limited. Nord Pool's terms of use apply to the data.

This project isn't affiliated with Nord Pool.
