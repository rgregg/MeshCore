# RAK4631 PoE Hardening + Node-Memory Network Config Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden the RAK4631 Ethernet variants for marginal-PoE boot and move the static-IP fallback from compile-time defines into node memory.

**Architecture:** A new compile-time flag `ETHERNET_POE` (implies `ETHERNET_ENABLED`) gates two power-hardening edits in the RAK4631 board init. Separately, `NodePrefs` gains persisted Ethernet config (DHCP toggle + static IP/gateway/subnet/DNS) consumed by both the repeater/room-server (`EthernetCLI`) and companion (`SerialEthernetInterface`) init paths.

**Tech Stack:** C++ / Arduino (Adafruit nRF52 core), PlatformIO, nRF52840 (RAK4631), W5100S (RAK13800), FreeRTOS.

## Global Constraints

- nRF52 / RAK4631 only — do not touch ESP32 or RP2040 Ethernet paths.
- Existing `*_ethernet` envs and stock `RAK_4631_repeater` must keep building and behaving unchanged (guards must not leak).
- Default Ethernet config is **DHCP** — upgrades of existing nodes must not change behavior.
- The boot-voltage bypass (Task 2) must be compiled in **only** under `ETHERNET_POE`, never a battery build.
- No unit-test harness exists for this hardware-coupled code; the verification gate per task is **compile** (`pio run -e <env>`) plus **on-device** checks via the established USB-DFU loop on `piserial5` where noted.
- Build tool: `~/.local/bin/pio`. PoE test device: SheridanBeachOne (USB `/dev/ttyACM1` on `piserial5.lan`, Ethernet `10.0.2.15:23`).
- No Claude/AI attribution in commit messages.

---

## Phase 1 — PoE power-hardening (flash-testable)

### Task 1: `ETHERNET_POE` flag + three `_poe` envs

**Files:**
- Modify: `src/helpers/nrf52/EthernetCLI.h:1-4` (implication guard)
- Modify: `variants/rak4631/platformio.ini` (three new envs)

**Interfaces:**
- Produces: compile-time macro `ETHERNET_POE`; ensures `ETHERNET_ENABLED` is defined whenever `ETHERNET_POE` is.

- [ ] **Step 1: Add the implication guard** at the very top of `src/helpers/nrf52/EthernetCLI.h`, before the existing `#ifdef ETHERNET_ENABLED` on line 3:

```cpp
#pragma once

// A PoE build is an Ethernet build plus power hardening.
#if defined(ETHERNET_POE) && !defined(ETHERNET_ENABLED)
  #define ETHERNET_ENABLED
#endif

#ifdef ETHERNET_ENABLED
```

- [ ] **Step 2: Add three `_poe` envs** to `variants/rak4631/platformio.ini`. Copy each existing `*_ethernet` env block and append `-D ETHERNET_POE` to its `build_flags`. Example for the repeater (place after `[env:RAK_4631_repeater_ethernet]`):

```ini
[env:RAK_4631_repeater_poe]
extends = env:RAK_4631_repeater_ethernet
build_flags =
  ${env:RAK_4631_repeater_ethernet.build_flags}
  -D ETHERNET_POE
```

Repeat with `RAK_4631_room_server_poe` (extends `RAK_4631_room_server_ethernet`) and `RAK_4631_companion_radio_poe` (extends `RAK_4631_companion_radio_ethernet`). If `extends` of a concrete `env:` is not supported in this PlatformIO version, instead duplicate the full env block and add the one `-D ETHERNET_POE` line.

- [ ] **Step 3: Build all three** to verify the flag and envs:

Run: `~/.local/bin/pio run -e RAK_4631_repeater_poe -e RAK_4631_room_server_poe -e RAK_4631_companion_radio_poe`
Expected: 3 × `[SUCCESS]`.

- [ ] **Step 4: Commit**

```bash
git add src/helpers/nrf52/EthernetCLI.h variants/rak4631/platformio.ini
git commit -m "Add ETHERNET_POE flag and RAK4631 _poe envs"
```

---

### Task 2: A — boot-voltage bootlock bypass

**Files:**
- Modify: `variants/rak4631/RAK4631Board.cpp:22-26` (`power_config`)

**Interfaces:**
- Consumes: `ETHERNET_POE` (Task 1); `NRF52Board::checkBootVoltage()` treats `voltage_bootlock == 0` as "protection disabled" (verified at `src/helpers/NRF52Board.cpp:104`).

- [ ] **Step 1: Edit `power_config`** in `variants/rak4631/RAK4631Board.cpp`:

```cpp
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel      = PWRMGT_LPCOMP_REFSEL,
#ifdef ETHERNET_POE
  // PoE = no battery. isExternalPowered() only detects USB VBUS, not PoE, so
  // checkBootVoltage() would read the floating battery ADC and SYSTEMOFF-loop
  // (red-LED flicker). 0 => boot protection disabled.
  .voltage_bootlock   = 0
#else
  .voltage_bootlock   = PWRMGT_VOLTAGE_BOOTLOCK
#endif
};
```

- [ ] **Step 2: Build PoE + regression env**

Run: `~/.local/bin/pio run -e RAK_4631_repeater_poe -e RAK_4631_repeater`
Expected: both `[SUCCESS]`.

- [ ] **Step 3: Confirm the gate didn't leak** — the stock `RAK_4631_repeater` build must still compile `voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK`. Verify by grepping the preprocessed guard is `ETHERNET_POE`-only (visual check of the diff).

- [ ] **Step 4: Commit**

```bash
git add variants/rak4631/RAK4631Board.cpp
git commit -m "Bypass boot-voltage protection on ETHERNET_POE builds"
```

---

### Task 3: B — drive W5100S reset HIGH in the early constructor

**Files:**
- Modify: `variants/rak4631/RAK4631Board.cpp:7-17` (`rak4631_early_poe_power`)

**Interfaces:**
- Consumes: `ETHERNET_POE`; W5100S reset = `PIN_ETHERNET_RESET` (21).

- [ ] **Step 1: Verify the pin mapping.** Confirm Arduino pin 21 maps to `P0.21` on this variant:

Run: `grep -nE 'g_ADigitalPinMap|21|P0_21|WB_IO3' variants/rak4631/variant.cpp variants/rak4631/variant.h 2>/dev/null | head`
Expected: pin index 21 in `g_ADigitalPinMap` resolves to port 0 pin 21 (`(0 + 21)` / `P0.21`). If it does **not**, use the actual port/pin in the `NRF_GPIO_PIN_MAP(port, pin)` call below.

- [ ] **Step 2: Extend the early constructor** in `variants/rak4631/RAK4631Board.cpp`:

```cpp
#ifdef ETHERNET_ENABLED
static void __attribute__((constructor(102))) rak4631_early_poe_power() {
  nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(1, 2));   // WB_IO2 = P1.02 (3V3 peripheral rail)
  nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(1, 2));
#ifdef ETHERNET_POE
  // Release the W5100S from reset as early as possible so the PHY draws its
  // ~120 mA and latches a marginal PoE converter before its foldback timer.
  nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 21));  // W5100S RST = P0.21 (WB_IO3)
  nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, 21));
#endif
}
#endif
```

- [ ] **Step 3: Build**

Run: `~/.local/bin/pio run -e RAK_4631_repeater_poe`
Expected: `[SUCCESS]`.

- [ ] **Step 4: Commit**

```bash
git add variants/rak4631/RAK4631Board.cpp
git commit -m "Release W5100S reset early in boot constructor on PoE builds"
```

---

### Task 4: Phase-1 on-device validation (SheridanBeachOne)

**Files:** none (validation only).

- [ ] **Step 1: Flash `RAK_4631_repeater_poe`** over the USB-DFU loop. From the repo root:

```bash
scp .pio/build/RAK_4631_repeater_poe/firmware.zip piserial5.lan:/tmp/sbo_repeater_poe.zip
ssh piserial5.lan 'NAME=$(sudo python3 -c "import serial,time;s=serial.Serial(\"/dev/ttyACM1\",115200,timeout=1);time.sleep(.3);s.reset_input_buffer();s.write(b\"get name\r\n\");time.sleep(1.2);import sys;sys.stdout.write(s.read(2000).decode(\"utf-8\",\"replace\"));s.close()"); echo "$NAME" | grep -q SheridanBeachOne && sudo adafruit-nrfutil --verbose dfu serial -p /dev/ttyACM1 -b 115200 --singlebank --touch 1200 -pkg /tmp/sbo_repeater_poe.zip | tail -6 || echo ABORT-wrong-port'
```

Expected: `Device programmed.`

- [ ] **Step 2: Confirm it booted and rejoined the wire**

```bash
ssh piserial5.lan '{ printf "ver\n"; sleep 2; } | nc -w8 10.0.2.15 23 | grep -iE "Build|CLI"'
```

Expected: `MeshCore Repeater CLI` + a `v1.16.0` build line (the node booted past brown-out with the PoE flags active).

- [ ] **Step 3: No commit** (validation only). If the device fails to boot, power-cycle and re-flash the last-known-good `RAK_4631_repeater_ethernet` image, then revisit Task 2/3.

---

## Phase 2 — Node-memory network config (repeater / room server)

### Task 5: `NodePrefs` Ethernet fields + persistence

**Files:**
- Modify: `src/helpers/CommonCLI.h:22-62` (struct), prefs-init defaults
- Modify: `src/helpers/CommonCLI.cpp` (`loadPrefsInt` tail, `savePrefs(fs)` tail)

**Interfaces:**
- Produces: `NodePrefs.eth_use_dhcp` (uint8_t, 1=DHCP), `eth_ip[4]`, `eth_gateway[4]`, `eth_subnet[4]`, `eth_dns[4]`.

- [ ] **Step 1: Append fields** to `struct NodePrefs` in `src/helpers/CommonCLI.h`, at the **end** of the struct (after the last existing member):

```cpp
  // Ethernet network config (node-memory). Default = DHCP.
  uint8_t eth_use_dhcp;   // 1 = DHCP (default), 0 = static
  uint8_t eth_ip[4];
  uint8_t eth_gateway[4];
  uint8_t eth_subnet[4];
  uint8_t eth_dns[4];
```

- [ ] **Step 2: Default to DHCP** wherever `NodePrefs` defaults are seeded (the same place `multi_acks`/`bw` etc. get their defaults — search `_prefs->multi_acks =` to locate it). Add:

```cpp
  _prefs->eth_use_dhcp = 1;
  memset(_prefs->eth_ip, 0, 4);
  memset(_prefs->eth_gateway, 0, 4);
  memset(_prefs->eth_subnet, 0, 4);
  memset(_prefs->eth_dns, 0, 4);
```

- [ ] **Step 3: Append reads** at the very end of `loadPrefsInt` (after the last `file.read(...)`), mirroring the existing pattern. Because trailing reads on a short (pre-upgrade) file leave the Step-2 defaults intact, this is backward-compatible:

```cpp
    file.read((uint8_t *)&_prefs->eth_use_dhcp, sizeof(_prefs->eth_use_dhcp));
    file.read((uint8_t *)_prefs->eth_ip, sizeof(_prefs->eth_ip));
    file.read((uint8_t *)_prefs->eth_gateway, sizeof(_prefs->eth_gateway));
    file.read((uint8_t *)_prefs->eth_subnet, sizeof(_prefs->eth_subnet));
    file.read((uint8_t *)_prefs->eth_dns, sizeof(_prefs->eth_dns));
```

- [ ] **Step 4: Append matching writes** at the very end of `savePrefs(fs)` (after the last `file.write(...)`):

```cpp
    file.write((uint8_t *)&_prefs->eth_use_dhcp, sizeof(_prefs->eth_use_dhcp));
    file.write((uint8_t *)_prefs->eth_ip, sizeof(_prefs->eth_ip));
    file.write((uint8_t *)_prefs->eth_gateway, sizeof(_prefs->eth_gateway));
    file.write((uint8_t *)_prefs->eth_subnet, sizeof(_prefs->eth_subnet));
    file.write((uint8_t *)_prefs->eth_dns, sizeof(_prefs->eth_dns));
```

- [ ] **Step 5: Build** (use a repeater Ethernet env that exercises CommonCLI):

Run: `~/.local/bin/pio run -e RAK_4631_repeater_ethernet -e RAK_4631_repeater`
Expected: both `[SUCCESS]`.

- [ ] **Step 6: Commit**

```bash
git add src/helpers/CommonCLI.h src/helpers/CommonCLI.cpp
git commit -m "Add Ethernet network config fields to NodePrefs"
```

---

### Task 6: CLI `set`/`get eth.*` handlers

**Files:**
- Modify: `src/helpers/CommonCLI.cpp` (`handleSetCmd`, `handleGetCmd`)

**Interfaces:**
- Consumes: `NodePrefs.eth_*` (Task 5).
- Produces: CLI verbs `set eth.dhcp {on|off}`, `set eth.ip A.B.C.D`, `set eth.gateway`, `set eth.subnet`, `set eth.dns`; `get eth.dhcp|eth.ip|eth.gateway|eth.subnet|eth.dns`.

- [ ] **Step 1: Add a small IPv4 parser helper** near the top of `CommonCLI.cpp` (file-local):

```cpp
// Parse "A.B.C.D" into out[4]; returns true on success.
static bool parseIPv4(const char* s, uint8_t out[4]) {
  int a, b, c, d;
  if (sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if ((a|b|c|d) < 0 || a > 255 || b > 255 || c > 255 || d > 255) return false;
  out[0]=a; out[1]=b; out[2]=c; out[3]=d; return true;
}
```

- [ ] **Step 2: Add `set` handlers** inside `handleSetCmd` (where `config = &command[4]`), following the existing `if (memcmp(config, "...", n) == 0)` chain. Add a branch:

```cpp
  } else if (memcmp(config, "eth.dhcp ", 9) == 0) {
    _prefs->eth_use_dhcp = (memcmp(&config[9], "on", 2) == 0) ? 1 : 0;
    savePrefs();
    sprintf(reply, "OK - eth.dhcp %s", _prefs->eth_use_dhcp ? "on" : "off");
  } else if (memcmp(config, "eth.ip ", 7) == 0) {
    if (parseIPv4(&config[7], _prefs->eth_ip)) { savePrefs(); strcpy(reply, "OK"); }
    else strcpy(reply, "ERROR: bad IPv4");
  } else if (memcmp(config, "eth.gateway ", 12) == 0) {
    if (parseIPv4(&config[12], _prefs->eth_gateway)) { savePrefs(); strcpy(reply, "OK"); }
    else strcpy(reply, "ERROR: bad IPv4");
  } else if (memcmp(config, "eth.subnet ", 11) == 0) {
    if (parseIPv4(&config[11], _prefs->eth_subnet)) { savePrefs(); strcpy(reply, "OK"); }
    else strcpy(reply, "ERROR: bad IPv4");
  } else if (memcmp(config, "eth.dns ", 8) == 0) {
    if (parseIPv4(&config[8], _prefs->eth_dns)) { savePrefs(); strcpy(reply, "OK"); }
    else strcpy(reply, "ERROR: bad IPv4");
```

- [ ] **Step 3: Add `get` handlers** inside `handleGetCmd`, following its existing `memcmp(config, ...)` chain:

```cpp
  } else if (memcmp(config, "eth.dhcp", 8) == 0) {
    sprintf(reply, "> %s", _prefs->eth_use_dhcp ? "on" : "off");
  } else if (memcmp(config, "eth.ip", 6) == 0) {
    sprintf(reply, "> %u.%u.%u.%u", _prefs->eth_ip[0], _prefs->eth_ip[1], _prefs->eth_ip[2], _prefs->eth_ip[3]);
  } else if (memcmp(config, "eth.gateway", 11) == 0) {
    sprintf(reply, "> %u.%u.%u.%u", _prefs->eth_gateway[0], _prefs->eth_gateway[1], _prefs->eth_gateway[2], _prefs->eth_gateway[3]);
  } else if (memcmp(config, "eth.subnet", 10) == 0) {
    sprintf(reply, "> %u.%u.%u.%u", _prefs->eth_subnet[0], _prefs->eth_subnet[1], _prefs->eth_subnet[2], _prefs->eth_subnet[3]);
  } else if (memcmp(config, "eth.dns", 7) == 0) {
    sprintf(reply, "> %u.%u.%u.%u", _prefs->eth_dns[0], _prefs->eth_dns[1], _prefs->eth_dns[2], _prefs->eth_dns[3]);
```

Note: order the `get` branches so longer keys (`eth.gateway`) are tested before shorter prefixes if any overlap; the keys above don't share prefixes except all start `eth.`, so place them after the existing chain in any order.

- [ ] **Step 4: Build**

Run: `~/.local/bin/pio run -e RAK_4631_repeater_ethernet`
Expected: `[SUCCESS]`.

- [ ] **Step 5: On-device CLI round-trip** (flash this build to SheridanBeachOne as in Task 4, then):

```bash
ssh piserial5.lan '{ printf "set eth.ip 10.0.2.99\n"; sleep 1; printf "get eth.ip\n"; sleep 1; } | nc -w6 10.0.2.15 23'
```

Expected: `OK` then `> 10.0.2.99`.

- [ ] **Step 6: Commit**

```bash
git add src/helpers/CommonCLI.cpp
git commit -m "Add set/get eth.* CLI commands for node-memory network config"
```

---

### Task 7: `EthernetCLI` consumes the network config

**Files:**
- Modify: `src/helpers/nrf52/EthernetCLI.h:35-85` (`ethernet_task`, `ethernet_start_task`)
- Modify: `examples/simple_repeater/main.cpp:109-110` and `examples/simple_room_server/main.cpp` (call site)

**Interfaces:**
- Consumes: `NodePrefs.eth_*` (Task 5).
- Produces: `ethernet_start_task(const uint8_t* mac_unused, bool use_dhcp, const uint8_t ip[4], const uint8_t gw[4], const uint8_t sn[4], const uint8_t dns[4])` — or a small static `EthernetConfig` set before `ethernet_start_task()`.

- [ ] **Step 1: Add a config holder** to `EthernetCLI.h` (file-local statics) above `ethernet_task`:

```cpp
static bool eth_use_dhcp = true;
static uint8_t eth_ip[4], eth_gw[4], eth_sn[4], eth_dns[4];

static void ethernet_set_config(bool use_dhcp, const uint8_t* ip, const uint8_t* gw,
                                const uint8_t* sn, const uint8_t* dns) {
  eth_use_dhcp = use_dhcp;
  memcpy(eth_ip, ip, 4); memcpy(eth_gw, gw, 4); memcpy(eth_sn, sn, 4); memcpy(eth_dns, dns, 4);
}
```

- [ ] **Step 2: Use the config** inside `ethernet_task`'s retry loop. Replace the DHCP-only `Ethernet.begin(mac, 10000, 2000)` block with:

```cpp
    if (eth_use_dhcp) {
      if (Ethernet.begin(mac, 10000, 2000) == 0) {
        if (Ethernet.hardwareStatus() == EthernetNoHardware) {
          Serial.println("ETH: Hardware not found, giving up");
          vTaskDelete(NULL); return;
        }
        Serial.println(Ethernet.linkStatus() == LinkOFF
          ? "ETH: Cable not connected, will retry" : "ETH: DHCP failed, will retry");
        vTaskDelay(pdMS_TO_TICKS(ETHERNET_RETRY_INTERVAL_MS));
        continue;
      }
    } else {
      IPAddress ip(eth_ip), gw(eth_gw), sn(eth_sn), dns(eth_dns);
      Ethernet.begin(mac, ip, dns, gw, sn);  // static: no return code
      if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("ETH: Hardware not found, giving up");
        vTaskDelete(NULL); return;
      }
    }
```

- [ ] **Step 3: Report mode** in `eth.status` (in `ethernet_handle_command`): append `" (dhcp)"` or `" (static)"` to the connected reply based on `eth_use_dhcp`.

- [ ] **Step 4: Set config at the call site.** In `examples/simple_repeater/main.cpp`, replace the `ethernet_start_task();` call (line ~110) with:

```cpp
#ifdef ETHERNET_ENABLED
  {
    auto* p = the_mesh.getNodePrefs();
    ethernet_set_config(p->eth_use_dhcp, p->eth_ip, p->eth_gateway, p->eth_subnet, p->eth_dns);
    ethernet_start_task();
  }
#endif
```

Apply the identical change at the matching `ethernet_start_task();` call site in `examples/simple_room_server/main.cpp`.

- [ ] **Step 5: Build both roles**

Run: `~/.local/bin/pio run -e RAK_4631_repeater_ethernet -e RAK_4631_room_server_ethernet -e RAK_4631_repeater_poe`
Expected: all `[SUCCESS]`.

- [ ] **Step 6: On-device static-IP test** (flash repeater build to SheridanBeachOne):

```bash
ssh piserial5.lan '{ printf "set eth.dhcp off\n"; sleep 1; printf "set eth.ip 10.0.2.99\n"; sleep 1; printf "set eth.gateway 10.0.0.1\n"; sleep 1; printf "set eth.subnet 255.255.252.0\n"; sleep 1; printf "set eth.dns 10.0.0.1\n"; sleep 1; printf "reboot\n"; sleep 2; } | nc -w8 10.0.2.15 23; sleep 25; { printf "eth.status\n"; sleep 2; } | nc -w8 10.0.2.99 23'
```

Expected: after reboot the node answers at `10.0.2.99` with `ETH: 10.0.2.99:23 (static)`. Then restore DHCP: `set eth.dhcp on` + `reboot`.

- [ ] **Step 7: Commit**

```bash
git add src/helpers/nrf52/EthernetCLI.h examples/simple_repeater/main.cpp examples/simple_room_server/main.cpp
git commit -m "Apply node-memory network config in EthernetCLI init"
```

---

## Phase 3 — Node-memory network config (companion)

### Task 8: Companion `NodePrefs` + `SerialEthernetInterface` runtime config

**Files:**
- Modify: `examples/companion_radio/NodePrefs.h` (struct)
- Modify: `examples/companion_radio/DataStore.cpp` (companion prefs load/save — locate the `file.read`/`file.write` sequence for `NodePrefs` and mirror Task 5's append)
- Modify: `src/helpers/nrf52/SerialEthernetInterface.cpp:begin()` and `.h`
- Modify: `examples/companion_radio/main.cpp` (pass config before `serial_interface.begin()`)

**Interfaces:**
- Consumes: companion `NodePrefs` (separate struct from `CommonCLI`).
- Produces: same `eth_use_dhcp`/`eth_ip`/`eth_gateway`/`eth_subnet`/`eth_dns` fields; `SerialEthernetInterface::setNetConfig(...)`.

- [ ] **Step 1: Append the same five fields** to the end of `struct NodePrefs` in `examples/companion_radio/NodePrefs.h` (identical declarations to Task 5 Step 1).

- [ ] **Step 2: Default + persist.** In `examples/companion_radio/DataStore.cpp`, find where companion `NodePrefs` are default-initialized and where they are read/written to file. Seed defaults (`eth_use_dhcp = 1`, zeros) before load, and append the five `read`/`write` calls at the end of the load and save sequences exactly as in Task 5 Steps 3–4.

- [ ] **Step 3: Add a runtime setter** to `SerialEthernetInterface` (`.h` declaration + `.cpp`):

```cpp
void SerialEthernetInterface::setNetConfig(bool use_dhcp, const uint8_t* ip,
    const uint8_t* gw, const uint8_t* sn, const uint8_t* dns) {
  _use_dhcp = use_dhcp;
  memcpy(_ip, ip, 4); memcpy(_gw, gw, 4); memcpy(_sn, sn, 4); memcpy(_dns, dns, 4);
}
```

Add members `bool _use_dhcp = true; uint8_t _ip[4], _gw[4], _sn[4], _dns[4];` to the class.

- [ ] **Step 4: Use runtime config in `begin()`.** Replace the `#if defined(ETHERNET_STATIC_IP) && ...` compile-time block with:

```cpp
  if (!_use_dhcp) {
    IPAddress ip(_ip), gw(_gw), sn(_sn), dns(_dns);
    Ethernet.begin(mac, ip, dns, gw, sn);
  } else {
    if (Ethernet.begin(mac) == 0) {
      if (Ethernet.hardwareStatus() == EthernetNoHardware) { ETHERNET_DEBUG_PRINTLN("Ethernet hardware not found."); return false; }
      if (Ethernet.linkStatus() == LinkOFF) { ETHERNET_DEBUG_PRINTLN("Ethernet cable not connected."); return false; }
      // (keep the remainder of the existing DHCP-failure handling here)
    }
  }
```

- [ ] **Step 5: Pass config at the call site** in `examples/companion_radio/main.cpp`, before the `serial_interface.begin()` call in the `ETHERNET_ENABLED` setup branch:

```cpp
  {
    auto* p = the_mesh.getNodePrefs();
    serial_interface.setNetConfig(p->eth_use_dhcp, p->eth_ip, p->eth_gateway, p->eth_subnet, p->eth_dns);
  }
```

- [ ] **Step 6: Build companion envs**

Run: `~/.local/bin/pio run -e RAK_4631_companion_radio_ethernet -e RAK_4631_companion_radio_poe`
Expected: both `[SUCCESS]`.

- [ ] **Step 7: Commit**

```bash
git add examples/companion_radio/NodePrefs.h examples/companion_radio/DataStore.cpp src/helpers/nrf52/SerialEthernetInterface.cpp src/helpers/nrf52/SerialEthernetInterface.h examples/companion_radio/main.cpp
git commit -m "Apply node-memory network config in companion Ethernet interface"
```

---

## Final — full build matrix + regression

### Task 9: Build matrix and regression check

**Files:** none (verification).

- [ ] **Step 1: Build every affected env**

Run:
```bash
~/.local/bin/pio run \
  -e RAK_4631_repeater_poe -e RAK_4631_room_server_poe -e RAK_4631_companion_radio_poe \
  -e RAK_4631_repeater_ethernet -e RAK_4631_room_server_ethernet -e RAK_4631_companion_radio_ethernet \
  -e RAK_4631_repeater -e RAK_4631_companion_radio_usb
```
Expected: all `[SUCCESS]`.

- [ ] **Step 2: Confirm guards didn't leak** — `RAK_4631_repeater` (stock) and the `_ethernet` envs build identically to before this branch (no `ETHERNET_POE` code paths). Spot-check the diff for any unguarded change to shared files.

- [ ] **Step 3: Final on-device pass** — leave SheridanBeachOne running the `RAK_4631_repeater_poe` build on DHCP, confirm it's reachable at `10.0.2.15:23` and forwarding.

- [ ] **Step 4: No commit** (verification). Branch is ready for PR / further testing.

---

## Self-Review

- **Spec coverage:** Flag implication (Task 1), envs ×3 (Task 1), A boot-voltage (Task 2), B early RST (Task 3), D.1 repeater/room prefs+CLI+init (Tasks 5–7), D.2 companion (Task 8), build/regression + hardware test (Tasks 4, 9). All spec sections mapped.
- **Open verifications carried from spec:** pin mapping (Task 3 Step 1), companion prefs save/load location (Task 8 Step 2) — both are concrete impl-time checks, not placeholders.
- **Type consistency:** field names `eth_use_dhcp/eth_ip/eth_gateway/eth_subnet/eth_dns` are identical across Tasks 5, 6, 7, 8; `ethernet_set_config` / `setNetConfig` signatures match their call sites.
