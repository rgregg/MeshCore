# RAK4631 PoE hardening + node-memory network config (`ETHERNET_POE`)

**Date:** 2026-06-30
**Branch:** `rak-ethernet-poe` (off `rak-ethernet`)
**Status:** Design — pending review

## Summary

Add a `rak-ethernet-poe` branch that hardens the RAK4631 Ethernet variants for
operation on a **marginal PoE supply** (RAK19018 / Silvertel Ag9905MT), and
moves the static-IP fallback from compile-time defines into **node memory** so
network config survives without reflashing.

All PoE-power changes are gated behind a new compile-time flag **`ETHERNET_POE`**,
which implies `ETHERNET_ENABLED`. Stock (battery / USB) Ethernet builds are
unaffected.

### Background / why

The RAK19018 PoE converter folds back into a pulsed mode when the load is below
~125–200 mA; a bare repeater draws a few mA, so the converter never latches and
the board never boots. The fix is to (a) get the W5100S drawing its ~120 mA as
early as possible, and (b) stop the firmware from shutting itself down on a
battery-less rail. Reference: competing PR meshcore-dev#2679.

What the `rak-ethernet` branch **already** does (no work needed):
- WB_IO2 (3V3 peripheral rail) driven HIGH in an early `constructor(102)`.
- W5100S init + DHCP run in a **background FreeRTOS task** (`ethernet_task` in
  `EthernetCLI.h`) with a non-blocking retry loop — so cold-start is not blocked
  by setup() and DHCP cannot reboot-loop the supply. (#2679's deferred-init and
  non-blocking-DHCP work is therefore already covered here.)
- Reset is deliberately not toggled (toggling kills the PHY link / PoE draw).

This branch adds the remaining gaps.

## Scope

In scope (all on this branch):
- **A.** Boot-voltage bootlock bypass under `ETHERNET_POE`.
- **B.** Drive the W5100S reset line HIGH in the early boot constructor.
- **D.** Node-memory network config (static IP / DHCP toggle in `NodePrefs` + CLI),
  replacing compile-time static defines, used by both the repeater/room-server
  (`EthernetCLI`) and companion (`SerialEthernetInterface`) init paths.
- **Envs.** `RAK_4631_repeater_poe`, `RAK_4631_room_server_poe`,
  `RAK_4631_companion_radio_poe`.

Out of scope:
- ESP32/RP2040 Ethernet. nRF52/RAK4631 only.
- Changes to the existing non-PoE `*_ethernet` envs' behavior.

## The `ETHERNET_POE` flag

- Defined in the new `_poe` env `build_flags`, alongside `ETHERNET_ENABLED`.
- A guard ensures the implication holds even if only `ETHERNET_POE` is set:
  in a shared Ethernet header (e.g. top of `EthernetCLI.h` / a small common
  include), `#if defined(ETHERNET_POE) && !defined(ETHERNET_ENABLED)` →
  `#define ETHERNET_ENABLED`.
- `ETHERNET_POE` gates power-hardening only (A, B). The node-memory config (D)
  is gated by `ETHERNET_ENABLED` so it benefits all Ethernet builds.

## A. Boot-voltage bootlock bypass

`variants/rak4631/RAK4631Board.cpp`, `power_config`:

```c
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel      = PWRMGT_LPCOMP_REFSEL,
#ifdef ETHERNET_POE
  // PoE = no battery; isExternalPowered() only detects USB VBUS, not PoE, so
  // checkBootVoltage() would read the floating battery ADC and SYSTEMOFF-loop.
  .voltage_bootlock   = 0,   // checkBootVoltage(): 0 => protection disabled
#else
  .voltage_bootlock   = PWRMGT_VOLTAGE_BOOTLOCK
#endif
};
```

Verified: `NRF52Board::checkBootVoltage()` returns `true` early when
`voltage_bootlock == 0`. Runtime PoE detection is not possible (no VBUS), so the
compile-time gate is the only correct mechanism.

## B. Early W5100S reset-high

Extend the existing `rak4631_early_poe_power()` `constructor(102)` to also drive
the W5100S reset line HIGH (it is currently driven later, inside `ethernet_task`
/ `SerialEthernetInterface::begin`). Raw nRF registers, since the Arduino GPIO
layer is not up that early.

```c
#ifdef ETHERNET_ENABLED
static void __attribute__((constructor(102))) rak4631_early_poe_power() {
  nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(1, 2));   // WB_IO2 (3V3 rail)
  nrf_gpio_pin_set (NRF_GPIO_PIN_MAP(1, 2));
#ifdef ETHERNET_POE
  nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 21));  // W5100S RST (P0.21 / WB_IO3)
  nrf_gpio_pin_set (NRF_GPIO_PIN_MAP(0, 21));
#endif
}
#endif
```

**To verify during implementation:** that Arduino pin `PIN_ETHERNET_RESET` (21)
maps to `P0.21` on this variant (cross-check `variant.h`). #2679 uses P0.21.

## D. Node-memory network config

Replaces compile-time static IP with runtime prefs. Default = DHCP (current
behavior), so existing deployments are unchanged on upgrade.

### D.1 Repeater / room server (shared `CommonCLI`)

`src/helpers/CommonCLI.h` — append to `struct NodePrefs`:

```c
uint8_t eth_use_dhcp;   // 1 = DHCP (default), 0 = static
uint8_t eth_ip[4];
uint8_t eth_gateway[4];
uint8_t eth_subnet[4];
uint8_t eth_dns[4];
```

- **Defaults:** set `eth_use_dhcp = 1` (and zeros) in the prefs-init path before
  load, so old/short pref files upgrade cleanly.
- **Persistence:** append matching `file.read(...)` / `file.write(...)` lines at
  the **end** of `loadPrefsInt`/`savePrefs(fs)`, following the established
  "new fields go to the end for upgrade compat" pattern (trailing reads on a
  short file leave defaults intact).
- **CLI:** add to `handleSetCmd`/`handleGetCmd`:
  - `set eth.dhcp {on|off}`
  - `set eth.ip A.B.C.D`, `set eth.gateway …`, `set eth.subnet …`, `set eth.dns …`
  - `get eth.*` mirrors. Each `set` calls `savePrefs()`.

`src/helpers/nrf52/EthernetCLI.h` — `ethernet_task` consumes the config:
- Plumb the config into the task. `ethernet_start_task()` gains parameters (or a
  small `EthernetConfig` struct copied into a static), populated from `_prefs`
  at the call site in `simple_repeater` / `simple_room_server` setup.
- If `eth_use_dhcp`: keep current `Ethernet.begin(mac, 10000, 2000)` retry loop.
- Else: `Ethernet.begin(mac, ip, dns, gateway, subnet)` and mark running.
- `eth.status` extended to report DHCP vs static.

### D.2 Companion (`SerialEthernetInterface`)

`SerialEthernetInterface::begin()` currently selects static vs DHCP via
`ETHERNET_STATIC_*` build defines. Change to take the same runtime config
(from the companion `NodePrefs` in `examples/companion_radio/NodePrefs.h`):
- Add equivalent `eth_*` fields + persistence + companion CLI get/set.
- `begin()` uses runtime values; compile-time `ETHERNET_STATIC_*` defines are
  removed (or kept only as the seed defaults).

### D.3 Interaction with `ETHERNET_POE`

Static config is independent of PoE, but is the recommended pairing for a PoE
node on a network without DHCP. No special-casing required.

## New PlatformIO envs

In `variants/rak4631/platformio.ini`, three envs extending the existing
`*_ethernet` envs, adding `-D ETHERNET_POE` (and keeping `-D ETHERNET_ENABLED`):

- `RAK_4631_repeater_poe`
- `RAK_4631_room_server_poe`
- `RAK_4631_companion_radio_poe`

Default network config = DHCP (node-memory). Optionally ship a documented
default static via first-boot prefs, but not required.

## Testing

- **Build:** all three `_poe` envs compile; the three existing `_ethernet` envs
  and stock `RAK_4631_repeater` still compile unchanged (guards don't leak).
- **Prefs upgrade:** a node with an old pref file loads with `eth_use_dhcp = 1`
  and unchanged behavior.
- **CLI:** `set eth.dhcp off` + `set eth.ip/gateway/subnet`, reboot, confirm the
  static address is applied; `set eth.dhcp on` restores DHCP.
- **Hardware:** flash `RAK_4631_repeater_poe` to SheridanBeachOne over the
  established USB-serial DFU loop (`adafruit-nrfutil dfu serial … --touch 1200`
  via piserial5), confirm it boots and rejoins the wire. PoE-specific
  brown-out behavior validated on a RAK19018 if available.

## Risks / open items

- **B pin mapping** — confirm Arduino 21 == P0.21 before relying on the raw
  register write.
- **Boot-voltage bypass is a safety reduction** — only ever compiled into a
  `_poe` build; never in battery builds. This is the reason it is a dedicated
  flag rather than `ETHERNET_ENABLED`.
- **Node-memory config is cross-cutting** (two prefs systems, two init paths).
  Sequence in the plan: A+B+envs first (fast, flash-testable), then D.1
  (repeater/room), then D.2 (companion).
- **Companion NodePrefs layout** — confirm its own save/load supports appended
  fields the same way `CommonCLI` does.
