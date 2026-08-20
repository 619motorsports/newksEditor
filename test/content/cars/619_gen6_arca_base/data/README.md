# NASCAR grille tape and engine thermal model

This is a CSP custom car-physics script combining adjustable grille tape,
density-sensitive aerodynamics, and a five-node real-time engine thermal model.

## What it models

- Grille tape from 0% to 100% as a setup item.
- A small configurable reduction to total car drag by scaling each `aero.ini` wing's `CD_GAIN`, without changing `CL_GAIN`.
- Independently tunable drag and downforce response to CSP's local air-density loss.
- Tape-dependent radiator and oil-cooler airflow.
- Proximity-based radiator and oil-cooler airflow loss in the wake of a car directly ahead.
- Cylinder head, block, coolant, oil, and exhaust-gas temperatures.
- Combustion/shaft-power heat input, heat soak, thermostat, RPM-dependent water pump, speed/air-density ram air, fan hysteresis, and oil thermostat.
- Hot/cold torque loss and permanent engine-life damage.
- CSP save-state/replay persistence and twelve physics-state outputs for dashboards or data logging.

## Installation

1. Unpack the target car's data with Content Manager if it only has `data.acd`.
2. Copy `script.lua` and `nascar_thermal.ini` into `<Assetto Corsa>/content/cars/<car>/data/`.
3. Append `setup_snippet.ini` to that car's `data/setup.ini`.
4. Repack the car data if required by your workflow, and enable CSP extended physics for the car/session.

The setup section name must have an unused numeric suffix: `[CUSTOM_SCRIPT_ITEM_0]`, then `_1`, `_2`, and so on if the car already defines custom script items. Keep `ID=nascar_grille_tape`; that is the identifier used by Lua. Do not add `EXT_CONTROLLER`: setup controllers intentionally hide the setup spinner and drive its value from a dynamic controller instead.

CSP automatically loads `data/script.lua` as the car's custom physics script. If the car already has that file, merge the initialization and the bodies of `script.update()`/`script.reset()` instead of overwriting it. There can only be one set of those callbacks.

The script reads every INI/config/setup value once at load. It only checks the cached setup reference during thermal steps.

## Debug logging

Debug logging is enabled in `nascar_thermal.ini` by default while diagnosing:

```ini
[DEBUG]
ENABLED=1
PERIOD_SECONDS=2.0
LUA_DEBUG_VALUES=1
LOG_AERO_WINGS=1
```

Each line starts with `[NASCAR_THERMAL]`. Initialization messages identify the car, CSP version, physics permission, setup-item result, effective tape source/value, detected aero wings, CD multiplier, and output channels. Rate-limited state messages then report temperatures, engine inputs, tape flow, front-car index/gap, traffic airflow, fan state, torque loss, life, and overheat severity.

On this Windows installation the persistent log is:

`C:\Users\Jackie\Documents\Assetto Corsa\logs\custom_shaders_patch.log`

You can also open CSP's in-game Lua Debug app to view the named `NASCAR Thermal/*` values. Set `DEBUG/ENABLED=0` after diagnosis; disabled debugging does not format strings or write logs.

The setup item takes precedence over `GRILLE_TAPE/DEFAULT_PERCENT`. If `data/setup.ini` contains the item with `DEFAULT=0`, the effective tape is 0% even if `DEFAULT_PERCENT` is nonzero. Either choose tape in the setup screen or change that setup `DEFAULT` value.

By default, `script.reset()` explicitly restores engine life to `INITIAL_STATE/ENGINE_LIFE=1000`. This avoids inheriting a zero `engineLifeLeft` value from the previous broken session. Set `INITIAL_STATE/REPAIR_ENGINE_ON_RESET=0` only if engine damage is intentionally meant to persist across resets and returns to the pits. The reset log reports the previous life, new life, and whether repair was enabled.

## Calibration

The defaults target a naturally aspirated NASCAR-style V8 near 500 kW. Start by calibrating these values in `nascar_thermal.ini`:

1. `HEAT_INPUT/RATED_POWER_KW` and `COMBUSTION_EFFICIENCY` for engine output and fuel efficiency.
2. `COOLING/RADIATOR_MAX_KW_PER_C` until coolant stabilizes near the desired temperature with 0% tape at race speed.
3. `COOLING/OIL_COOLER_MAX_KW_PER_C` for oil temperature.
4. `GRILLE_TAPE/MIN_COOLING_FLOW` and `COOLING_FLOW_EXPONENT` to set how strongly tape hurts cooling.
5. `GRILLE_TAPE/MAX_DRAG_REDUCTION` for the maximum total drag benefit. `0.025` means 2.5% at 100% tape.
6. `AERODYNAMICS/DRAG_DENSITY_RESPONSE` and `DOWNFORCE_DENSITY_RESPONSE` for aerodynamic wake strength.
7. `TRAFFIC_COOLING/MIN_AIRFLOW`, full/max gap, and lateral limits for wake cooling loss.
8. Warning/critical temperatures and damage rates last.

Because `ac.setWingGain()` sets absolute gains, this implementation is intended
for cars with static `CD_GAIN`/`CL_GAIN` values. If another dynamic system also
changes wing gains, combine its multiplier with `tapeCdMultiplier`,
`aeroDragMultiplier`, or `aeroDownforceMultiplier` in `updateAerodynamics()`.

### Density-sensitive aerodynamics

The aerodynamic model compares `physicsCar.airDensity` with the ambient density
calculated from the air pressure and temperature at the car:

```lua
local DensityRatio = (math.min((math.floor((CarPhysics.airDensity/((ac.getAirPressure(CarPhysics.position)*1000) / ((CarPhysics.ambientTemperature+273.15)*287.058))) * (1000) + 0.5)) / (1000),1))
```

The result is rounded to `0.001` and capped at `1`. A ratio of `1` leaves the
base wing gains unchanged. Lower ratios modify drag and downforce independently:

```text
gain multiplier = 1 - (1 - DensityRatio) * DENSITY_RESPONSE
```

Set a response to `0` to retain only CSP's native air-density effect. `1` adds
a one-for-one gain reduction, values above `1` strengthen it, and negative
values compensate for some of the native loss. `MIN_GAIN_MULTIPLIER` and
`MAX_GAIN_MULTIPLIER` bound both results. The grille-tape drag multiplier is
then combined with the density-based drag multiplier, so both features can be
tuned without overwriting each other.

`AERODYNAMICS/UPDATE_HZ` controls how often the density calculation is sampled;
the default is 10 Hz and it cannot exceed `MODEL/UPDATE_HZ`.
`RATIO_CHANGE_EPSILON` prevents wing-gain writes until the sampled ratio has
changed by at least that amount. The density function is also excluded from
LuaJIT compilation when CSP exposes the `jit` control table. These safeguards
avoid a CSP 0.3.0.520 trace-compiler panic seen when the original implementation
ran the density query in every physics callback for every car in a large field.

## Physics outputs

The default output base is 240. Other Lua scripts can read these as `scriptControllerInputs`; dynamic controllers and emissives can read `CPHYS_SCRIPT_240` through `CPHYS_SCRIPT_251`.

These are valid CSP instrument inputs and can be used anywhere the extension-config instrument system accepts `INPUT`, including analog indicators, animations and emissives. For `digital_instruments.ini`, use the same name with `TYPE_EXT`. See `instrument_inputs_snippet.ini` for ready-to-use replacements for this car's water and oil gauges.

| Offset | Default input | Value |
|---:|---:|---|
| 0 | 240 | Grille tape, 0 to 1 |
| 1 | 241 | Coolant temperature, C |
| 2 | 242 | Oil temperature, C |
| 3 | 243 | Cylinder-head temperature, C |
| 4 | 244 | Exhaust-gas temperature, C |
| 5 | 245 | Block temperature, C |
| 6 | 246 | Tape-dependent cooling-flow multiplier, 0 to 1 |
| 7 | 247 | Thermal torque loss, 0 to 1 |
| 8 | 248 | Engine life, 0 to 1 |
| 9 | 249 | Cooling fan active, 0 or 1 |
| 10 | 250 | Front-car bumper gap in metres, or -1 if no qualifying car |
| 11 | 251 | Traffic-wake airflow multiplier, 0 to 1 |

Change `MODEL/CONTROLLER_OUTPUT_BASE` if those channels conflict with an existing car script. The permitted base range is 0 through 244.

### Traffic-wake cooling

At `TRAFFIC_COOLING/SCAN_HZ`, the script projects every other active car onto this car's forward, side, and up vectors. A car only qualifies if it is ahead, laterally close, vertically close, and travelling in roughly the same direction. The strongest qualifying wake is selected. Distance, lateral offset, and heading alignment are smoothly combined, then `MIN_AIRFLOW` sets the worst-case remaining ram air.

With the defaults, a centered car no more than 3 m bumper-to-bumper leaves 20% of speed-dependent radiator and oil-cooler airflow. The effect fades out by 30 m. Natural radiator convection and the electric fan are not blocked. Grille tape and traffic wake multiply together for the ram-air path; for example, tape flow `0.50` and traffic airflow `0.40` produce effective ram flow `0.20`.

CSP documents `physicsCar.airDensity` as already being affected by its aerodynamic slipstream. This model uses the calculated local-to-ambient `DensityRatio` for ram air and then applies the explicit thermal-wake multiplier, so the effects compound intentionally. If your CSP/car combination already produces a strong density reduction, raise `MIN_AIRFLOW`, shorten `MAX_EFFECT_GAP_M`, or set `TRAFFIC_COOLING/ENABLED=0`.

`CPHYS_SCRIPT_250` and `CPHYS_SCRIPT_251`, plus the Lua Debug values `Front car gap m` and `Traffic airflow`, are intended for calibration. The detected gap uses `VEHICLE_LENGTH_M` as a reference-point-to-bumper approximation; tune it for unusually short or long cars. A gap of `-1` means no car passed all gates.

### Built-in temperature inputs

Modeled coolant is isolated from AC's standard `WATER_TEMPERATURE` state. Every physics frame, the script calls `ac.setWaterTemperature(SAFE_GAME_WATER_TEMPERATURE_C, true)`: the configured value is held at 90 C by default and the second argument disables AC's original water-temperature update logic. Use `CPHYS_SCRIPT_241` for the actual modeled water gauge.

This prevents AC's independent water-temperature damage from bypassing `nascar_thermal.ini` thresholds. `SAFE_GAME_WATER_TEMPERATURE_C` is clamped to 0 through 100 C in Lua so a configuration typo cannot create native overheating.

AC's current `engineLifeLeft` is authoritative during a stint, so native mechanical damage such as over-rev or turbo damage remains intact. The script no longer compares that value with a private previous-life value, logs native loss as an anomaly, or writes engine life when custom thermal damage is zero. When this model does have a nonzero `damageRate`, it subtracts only that step's thermal loss from AC's current value and writes the result through `ac.setEngineLifeLeft()`.

CSP's car-physics API does not expose a setter for the built-in estimated `OIL_TEMPERATURE`/`OIL_TEMP` or `EXHAUST_TEMPERATURE`/`EXHAUST_TEMP` values. Use `CPHYS_SCRIPT_242` for oil and `CPHYS_SCRIPT_244` for exhaust. Head and block temperatures use `CPHYS_SCRIPT_243` and `CPHYS_SCRIPT_245` respectively.

### Damage threshold semantics

Each hot-temperature node has three thresholds:

- `WARNING`: progressive torque derating begins; permanent hot damage remains zero.
- `CRITICAL`: permanent hot damage begins at zero and rises above this point.
- `FAILURE`: damage severity reaches 1.0 and life loss reaches `LIFE_LOSS_AT_FAILURE_PER_S`; it accelerates beyond failure using `DAMAGE_EXPONENT`.

The log reports `derateSeverity`, `derateSource`, `damageSeverity`, `damageRate`, and `damageSource` separately. For example, oil above `OIL_WARNING_C` but below `OIL_CRITICAL_C` can derate the engine but cannot reduce life through the custom hot-damage curve.

## Real-time characteristics

The thermal integrator is a fixed-step solver running at 20 Hz by default, while torque output remains at physics rate. Density-sensitive aero and traffic are sampled at 10 Hz by default. Traffic scanning uses scalar projections without creating temporary vectors. All INI access, aero discovery, setup lookup, and table creation happen during initialization. The physics callback creates no tables or vectors, logs nothing, and limits catch-up work after a pause to avoid a physics-thread spike.
