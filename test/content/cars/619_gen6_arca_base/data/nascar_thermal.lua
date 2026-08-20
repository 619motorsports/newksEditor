-- NASCAR engine thermal module. Loaded by data/script.lua.
local M = {}


local physicsCar = ac.accessCarPhysics()
local carIndex = car.index

local min = math.min
local max = math.max
local abs = math.abs
local sqrt = math.sqrt
local exp = math.exp
local format = string.format

local function clamp(value, lower, upper)
  return min(upper, max(lower, value))
end

local settings = ac.INIConfig.carData(carIndex, 'nascar_thermal.ini')

local function setting(section, key, fallback)
  return settings:get(section, key, fallback)
end

-- Solver settings. Thermal state is deliberately updated much more slowly than
-- the ~333 Hz physics callback; outputs affecting torque remain physics-rate.
local modelHz = clamp(setting('MODEL', 'UPDATE_HZ', 20), 5, 100)
local modelDt = 1 / modelHz
local maxSubsteps = clamp(setting('MODEL', 'MAX_SUBSTEPS', 4), 1, 10)
local outputBase = clamp(setting('MODEL', 'CONTROLLER_OUTPUT_BASE', 240), 0, 244)
local safeGameWaterTemperature = clamp(setting('OUTPUTS', 'SAFE_GAME_WATER_TEMPERATURE_C', 90), 0, 100)

-- Debugging is fully optional. When disabled, the only hot-path cost is a
-- single boolean check at thermal rate (not at the ~333 Hz physics rate).
local debugEnabled = setting('DEBUG', 'ENABLED', false)
local debugPeriod = max(0.25, setting('DEBUG', 'PERIOD_SECONDS', 2.0))
local debugLiveValues = setting('DEBUG', 'LUA_DEBUG_VALUES', true)
local logPrefix = '[NASCAR_THERMAL] '

local function debugLog(message)
  if debugEnabled then ac.log(logPrefix .. message) end
end

local function debugWarn(message)
  if debugEnabled then ac.warn(logPrefix .. message) end
end


-- Traffic wake settings. Other cars are sampled at a low rate because their
-- public StateCar positions update at graphics rate, while the resulting
-- airflow multiplier is smoothed at the thermal solver rate.
local trafficEnabled = setting('TRAFFIC_COOLING', 'ENABLED', true)
local trafficScanHz = clamp(setting('TRAFFIC_COOLING', 'SCAN_HZ', 10), 1, modelHz)
local trafficScanInterval = 1 / trafficScanHz
local trafficFullEffectGap = max(0, setting('TRAFFIC_COOLING', 'FULL_EFFECT_GAP_M', 3.0))
local trafficMaxGap = max(trafficFullEffectGap + 0.1,
  setting('TRAFFIC_COOLING', 'MAX_EFFECT_GAP_M', 30.0))
local trafficVehicleLength = max(0, setting('TRAFFIC_COOLING', 'VEHICLE_LENGTH_M', 5.0))
local trafficFullEffectLateral = max(0, setting('TRAFFIC_COOLING', 'FULL_EFFECT_LATERAL_M', 1.0))
local trafficMaxLateral = max(trafficFullEffectLateral + 0.1,
  setting('TRAFFIC_COOLING', 'MAX_LATERAL_M', 2.5))
local trafficMaxVertical = max(0.1, setting('TRAFFIC_COOLING', 'MAX_VERTICAL_M', 3.0))
local trafficMinDirectionDot = clamp(setting('TRAFFIC_COOLING', 'MIN_DIRECTION_DOT', 0.70), -1, 0.99)
local trafficMinimumAirflow = clamp(setting('TRAFFIC_COOLING', 'MIN_AIRFLOW', 0.20), 0, 1)
local trafficStrengthExponent = max(0.1, setting('TRAFFIC_COOLING', 'STRENGTH_EXPONENT', 1.40))
local trafficResponseSeconds = max(0.01, setting('TRAFFIC_COOLING', 'RESPONSE_SECONDS', 0.20))

-- Energy model values use kW, kJ/C and kW/C. Because 1 kW = 1 kJ/s,
-- temperature change is simply power * dt / heat capacity.
local combustionEfficiency = clamp(setting('HEAT_INPUT', 'COMBUSTION_EFFICIENCY', 0.34), 0.10, 0.60)
local idleFuelPower = max(0, setting('HEAT_INPUT', 'IDLE_FUEL_POWER_KW', 35))
local ratedPower = max(1, setting('HEAT_INPUT', 'RATED_POWER_KW', 500))
local heatToHead = max(0, setting('HEAT_INPUT', 'WASTE_TO_HEAD', 0.31))
local heatToBlock = max(0, setting('HEAT_INPUT', 'WASTE_TO_BLOCK', 0.13))
local heatToOil = max(0, setting('HEAT_INPUT', 'WASTE_TO_OIL', 0.10))

local headCapacity = max(1, setting('THERMAL_MASS', 'HEAD_KJ_PER_C', 160))
local blockCapacity = max(1, setting('THERMAL_MASS', 'BLOCK_KJ_PER_C', 700))
local coolantCapacity = max(1, setting('THERMAL_MASS', 'COOLANT_KJ_PER_C', 260))
local oilCapacity = max(1, setting('THERMAL_MASS', 'OIL_KJ_PER_C', 110))

local kHeadCoolant = max(0, setting('CONDUCTION', 'HEAD_TO_COOLANT_KW_PER_C', 7.0))
local kBlockCoolant = max(0, setting('CONDUCTION', 'BLOCK_TO_COOLANT_KW_PER_C', 2.5))
local kHeadBlock = max(0, setting('CONDUCTION', 'HEAD_TO_BLOCK_KW_PER_C', 1.6))
local kBlockOil = max(0, setting('CONDUCTION', 'BLOCK_TO_OIL_KW_PER_C', 0.8))
local kOilCoolant = max(0, setting('CONDUCTION', 'OIL_TO_COOLANT_KW_PER_C', 0.5))
local kExhaustHead = max(0, setting('CONDUCTION', 'EXHAUST_TO_HEAD_KW_PER_C', 0.035))
local kHeadAir = max(0, setting('CONDUCTION', 'HEAD_TO_AIR_KW_PER_C', 0.025))
local kBlockAir = max(0, setting('CONDUCTION', 'BLOCK_TO_AIR_KW_PER_C', 0.040))

local thermostatOpen = setting('COOLING', 'THERMOSTAT_OPEN_C', 76)
local thermostatFull = max(thermostatOpen + 0.1, setting('COOLING', 'THERMOSTAT_FULL_C', 86))
local radiatorMaxUA = max(0, setting('COOLING', 'RADIATOR_MAX_KW_PER_C', 10.0))
local radiatorNaturalUA = max(0, setting('COOLING', 'RADIATOR_NATURAL_KW_PER_C', 0.10))
local radiatorFanUA = max(0, setting('COOLING', 'FAN_KW_PER_C', 0.85))
local fanOnTemperature = setting('COOLING', 'FAN_ON_C', 100)
local fanOffTemperature = min(fanOnTemperature, setting('COOLING', 'FAN_OFF_C', 94))
local referenceAirSpeed = max(1, setting('COOLING', 'REFERENCE_AIR_SPEED_MPS', 75))
local ramAirExponent = max(0.1, setting('COOLING', 'RAM_AIR_EXPONENT', 0.65))
local idlePumpFraction = clamp(setting('COOLING', 'IDLE_PUMP_FRACTION', 0.35), 0, 1)
local oilThermostatOpen = setting('COOLING', 'OIL_THERMOSTAT_OPEN_C', 90)
local oilThermostatFull = max(oilThermostatOpen + 0.1, setting('COOLING', 'OIL_THERMOSTAT_FULL_C', 105))
local oilCoolerMaxUA = max(0, setting('COOLING', 'OIL_COOLER_MAX_KW_PER_C', 1.20))

local egtIdleRise = max(0, setting('EXHAUST', 'IDLE_RISE_C', 360))
local egtLoadRise = max(0, setting('EXHAUST', 'LOAD_RISE_C', 520))
local egtResponseSeconds = max(0.05, setting('EXHAUST', 'RESPONSE_SECONDS', 1.6))

local coolantWarning = setting('DAMAGE', 'COOLANT_WARNING_C', 108)
local coolantCritical = max(coolantWarning + 0.1, setting('DAMAGE', 'COOLANT_CRITICAL_C', 125))
local coolantFailure = max(coolantCritical + 0.1, setting('DAMAGE', 'COOLANT_FAILURE_C', 150))
local oilWarning = setting('DAMAGE', 'OIL_WARNING_C', 135)
local oilCritical = max(oilWarning + 0.1, setting('DAMAGE', 'OIL_CRITICAL_C', 155))
local oilFailure = max(oilCritical + 0.1, setting('DAMAGE', 'OIL_FAILURE_C', 175))
local headWarning = setting('DAMAGE', 'HEAD_WARNING_C', 155)
local headCritical = max(headWarning + 0.1, setting('DAMAGE', 'HEAD_CRITICAL_C', 190))
local headFailure = max(headCritical + 0.1, setting('DAMAGE', 'HEAD_FAILURE_C', 220))
local egtWarning = setting('DAMAGE', 'EGT_WARNING_C', 930)
local egtCritical = max(egtWarning + 0.1, setting('DAMAGE', 'EGT_CRITICAL_C', 1030))
local egtFailure = max(egtCritical + 0.1, setting('DAMAGE', 'EGT_FAILURE_C', 1150))
-- Keep the old key as a fallback for existing per-car configurations.
local legacyDamageRate = max(0, setting('DAMAGE', 'LIFE_LOSS_AT_CRITICAL_PER_S', 1.5))
local damageAtFailure = max(0, setting('DAMAGE', 'LIFE_LOSS_AT_FAILURE_PER_S', legacyDamageRate))
local damageExponent = max(1, setting('DAMAGE', 'DAMAGE_EXPONENT', 2.0))
local coldOilLimit = setting('DAMAGE', 'COLD_OIL_LIMIT_C', 45)
local coldDamageLoad = clamp(setting('DAMAGE', 'COLD_DAMAGE_LOAD', 0.75), 0, 1)
local coldDamageRate = max(0, setting('DAMAGE', 'COLD_LIFE_LOSS_PER_S', 0.08))
local maxHotTorqueLoss = clamp(setting('DAMAGE', 'MAX_HOT_TORQUE_LOSS', 0.30), 0, 0.75)
local maxColdTorqueLoss = clamp(setting('DAMAGE', 'MAX_COLD_TORQUE_LOSS', 0.04), 0, 0.25)
local coldDerateEnd = max(coldOilLimit + 0.1, setting('DAMAGE', 'COLD_DERATE_END_C', 75))

local startPrewarmed = setting('INITIAL_STATE', 'PREWARMED', true)
local startCoolant = setting('INITIAL_STATE', 'COOLANT_C', 80)
local startOil = setting('INITIAL_STATE', 'OIL_C', 85)
local startBlock = setting('INITIAL_STATE', 'BLOCK_C', 85)
local startHead = setting('INITIAL_STATE', 'HEAD_C', 90)
local startExhaust = setting('INITIAL_STATE', 'EXHAUST_C', 250)
local startEngineLife = clamp(setting('INITIAL_STATE', 'ENGINE_LIFE', 1000), 0, 1000)
local repairEngineOnReset = setting('INITIAL_STATE', 'REPAIR_ENGINE_ON_RESET', true)

-- All settings are scalar-cached above, so the parsed custom INI can be released.
settings = nil
setting = nil

local sim = ac.getSim()

local bridge = {
  getTapeFraction = function() return 0 end,
  getCoolingFlow = function() return 1 end,
  getDensityRatio = function() return 1 end
}

if debugEnabled then ac.setLogSilent(false) end

local headTemperature = 0
local blockTemperature = 0
local coolantTemperature = 0
local oilTemperature = 0
local exhaustTemperature = 0
local engineLife = 1000
local torqueLoss = 0
local overheatSeverity = 0
local damageSeverity = 0
local damageRate = 0
local damageSource = 'none'
local derateSource = 'none'
local fanActive = false
local accumulator = 0
local debugAccumulator = 0
local trafficScanAccumulator = trafficScanInterval
local frontCarGap = -1
local frontCarIndex = -1
local trafficAirflowTarget = 1
local trafficAirflow = 1

local function resetThermalState()
  local ambient = physicsCar.ambientTemperature
  local previousEngineLife = physicsCar.engineLifeLeft

  if startPrewarmed then
    coolantTemperature = max(ambient, startCoolant)
    oilTemperature = max(ambient, startOil)
    blockTemperature = max(ambient, startBlock)
    headTemperature = max(ambient, startHead)
    exhaustTemperature = max(ambient, startExhaust)
  else
    coolantTemperature = ambient
    oilTemperature = ambient
    blockTemperature = ambient
    headTemperature = ambient
    exhaustTemperature = ambient
  end

  -- AC can still report the previous session's zero engine life while the
  -- custom-physics reset callback is running. Explicitly seed and write a new
  -- engine here instead of adopting that stale broken state.
  if repairEngineOnReset then
    engineLife = startEngineLife
    ac.setEngineLifeLeft(engineLife)
  else
    engineLife = previousEngineLife
  end
  torqueLoss = 0
  overheatSeverity = 0
  damageSeverity = 0
  damageRate = 0
  damageSource = 'none'
  derateSource = 'none'
  fanActive = false
  accumulator = 0
  debugAccumulator = 0
  trafficScanAccumulator = trafficScanInterval
  frontCarGap = -1
  frontCarIndex = -1
  trafficAirflowTarget = 1
  trafficAirflow = 1
  ac.setWaterTemperature(safeGameWaterTemperature, true)

  if debugEnabled then
    debugLog(format('thermal reset ambient=%.1f coolant=%.1f oil=%.1f head=%.1f block=%.1f exhaust=%.1f life=%.1f previousLife=%.1f repaired=%s',
      ambient, coolantTemperature, oilTemperature, headTemperature,
      blockTemperature, exhaustTemperature, engineLife, previousEngineLife,
      tostring(repairEngineOnReset)))
  end
end

local function smoothRange(value, startValue, endValue)
  local x = clamp((value - startValue) / (endValue - startValue), 0, 1)
  return x * x * (3 - 2 * x)
end

local function severityAbove(value, warning, critical)
  return max(0, (value - warning) / (critical - warning))
end

local function scanTrafficAhead()
  local carsCount = sim.carsCount
  if not trafficEnabled or carsCount <= 1 then
    frontCarGap = -1
    frontCarIndex = -1
    trafficAirflowTarget = 1
    return
  end

  local ownPosition = physicsCar.position
  local ownLook = physicsCar.look
  local ownSide = physicsCar.side
  local ownUp = physicsCar.up
  local bestStrength = 0
  local bestGap = -1
  local bestIndex = -1

  for i = 0, carsCount - 1 do
    if i ~= carIndex then
      local other = ac.getCar(i)
      if other and other.isActive and other.isConnected then
        local otherPosition = other.position
        local dx = otherPosition.x - ownPosition.x
        local dy = otherPosition.y - ownPosition.y
        local dz = otherPosition.z - ownPosition.z
        local forward = dx * ownLook.x + dy * ownLook.y + dz * ownLook.z

        if forward > 0 then
          -- Approximate bumper-to-bumper gap from the cars' reference points.
          local gap = max(0, forward - trafficVehicleLength)
          if gap <= trafficMaxGap then
            local lateral = abs(dx * ownSide.x + dy * ownSide.y + dz * ownSide.z)
            local vertical = abs(dx * ownUp.x + dy * ownUp.y + dz * ownUp.z)

            if lateral < trafficMaxLateral and vertical < trafficMaxVertical then
              local otherLook = other.look
              local directionDot = otherLook.x * ownLook.x + otherLook.y * ownLook.y + otherLook.z * ownLook.z

              if directionDot > trafficMinDirectionDot then
                local distanceStrength = 1 - smoothRange(gap, trafficFullEffectGap, trafficMaxGap)
                local lateralStrength = 1
                if lateral > trafficFullEffectLateral then
                  lateralStrength = 1 - smoothRange(lateral, trafficFullEffectLateral, trafficMaxLateral)
                end
                local directionStrength = smoothRange(directionDot, trafficMinDirectionDot, 1)
                local strength = (distanceStrength * lateralStrength * directionStrength) ^ trafficStrengthExponent

                if strength > bestStrength or (strength == bestStrength and (bestGap < 0 or gap < bestGap)) then
                  bestStrength = strength
                  bestGap = gap
                  bestIndex = i
                end
              end
            end
          end
        end
      end
    end
  end

  frontCarGap = bestGap
  frontCarIndex = bestIndex
  trafficAirflowTarget = 1 - (1 - trafficMinimumAirflow) * bestStrength
end

local function updateTrafficCooling(dt)
  trafficScanAccumulator = trafficScanAccumulator + dt
  if trafficScanAccumulator >= trafficScanInterval then
    trafficScanAccumulator = trafficScanAccumulator % trafficScanInterval
    scanTrafficAhead()
  end

  local response = 1 - exp(-dt / trafficResponseSeconds)
  trafficAirflow = trafficAirflow + (trafficAirflowTarget - trafficAirflow) * response
  trafficAirflow = clamp(trafficAirflow, trafficMinimumAirflow, 1)
end

local function publishState()
  local outputs = physicsCar.controllerInputs
  -- Instrument-facing outputs. At the default base these are available to
  -- extension config as CPHYS_SCRIPT_240 through CPHYS_SCRIPT_251.
  outputs[outputBase] = bridge.getTapeFraction()
  outputs[outputBase + 1] = coolantTemperature
  outputs[outputBase + 2] = oilTemperature
  outputs[outputBase + 3] = headTemperature
  outputs[outputBase + 4] = exhaustTemperature
  outputs[outputBase + 5] = blockTemperature
  outputs[outputBase + 6] = bridge.getCoolingFlow()
  outputs[outputBase + 7] = torqueLoss
  outputs[outputBase + 8] = engineLife * 0.001
  outputs[outputBase + 9] = fanActive and 1 or 0
  outputs[outputBase + 10] = frontCarGap
  outputs[outputBase + 11] = trafficAirflow
end

local function publishDebug(dt)
  if not debugEnabled then return end
  debugAccumulator = debugAccumulator + dt
  if debugAccumulator < debugPeriod then return end
  debugAccumulator = debugAccumulator % debugPeriod

  local tapeFraction = bridge.getTapeFraction()
  local coolingFlow = bridge.getCoolingFlow()
  ac.log(logPrefix .. format(
    'thermal tape=%.1f%% densityRatio=%.3f rpm=%.0f speed=%.1fkm/h coolant=%.1f oil=%.1f head=%.1f block=%.1f egt=%.1f fan=%s frontCar=%d frontGap=%.2fm trafficAirflow=%.3f torqueLoss=%.4f damageRate=%.4f/s life=%.1f',
    tapeFraction * 100, bridge.getDensityRatio(), physicsCar.rpm,
    physicsCar.speedKmh, coolantTemperature, oilTemperature, headTemperature,
    blockTemperature, exhaustTemperature, tostring(fanActive), frontCarIndex,
    frontCarGap, trafficAirflow, torqueLoss, damageRate, engineLife))

  if debugLiveValues then
    ac.debug('NASCAR Thermal/Tape %', tapeFraction * 100)
    ac.debug('NASCAR Thermal/Coolant C', coolantTemperature)
    ac.debug('NASCAR Thermal/Native water C', safeGameWaterTemperature)
    ac.debug('NASCAR Thermal/Oil C', oilTemperature)
    ac.debug('NASCAR Thermal/Head C', headTemperature)
    ac.debug('NASCAR Thermal/Block C', blockTemperature)
    ac.debug('NASCAR Thermal/EGT C', exhaustTemperature)
    ac.debug('NASCAR Thermal/Cooling flow', coolingFlow)
    ac.debug('NASCAR Thermal/Front car index', frontCarIndex)
    ac.debug('NASCAR Thermal/Front car gap m', frontCarGap)
    ac.debug('NASCAR Thermal/Traffic airflow', trafficAirflow)
    ac.debug('NASCAR Thermal/Torque loss', torqueLoss)
    ac.debug('NASCAR Thermal/Damage rate per s', damageRate)
    ac.debug('NASCAR Thermal/Engine life', engineLife)
  end
end

local function thermalStep(dt)
  updateTrafficCooling(dt)

  local ambient = physicsCar.ambientTemperature
  local rpm = max(0, physicsCar.rpm)
  local rpmFraction = clamp(rpm / max(1000, physicsCar.rpmLimit), 0, 1.25)
  local load = clamp(physicsCar.gas, 0, 1)
  local shaftPower = max(0, physicsCar.engineTorque * physicsCar.engineVelocity * 0.001)
  local powerLoad = clamp(shaftPower / ratedPower, 0, 1)
  local effectiveLoad = max(load, powerLoad)

  -- Fuel energy inferred from delivered shaft power, plus the energy needed to
  -- keep a running engine idling. The remainder becomes rejected heat.
  local running = clamp(rpm / 600, 0, 1)
  local fuelPower = shaftPower / combustionEfficiency + idleFuelPower * running
  local rejectedHeat = max(0, fuelPower - shaftPower)
  local headInput = rejectedHeat * heatToHead
  local blockInput = rejectedHeat * heatToBlock
  local oilInput = rejectedHeat * heatToOil

  -- EGT is a fast gas-path node. Its target depends on both cylinder load and
  -- engine speed, while the metal/fluid nodes below conserve transferred heat.
  local egtTarget = ambient + egtIdleRise * running + egtLoadRise * effectiveLoad * (0.35 + 0.65 * rpmFraction)
  exhaustTemperature = exhaustTemperature + (egtTarget - exhaustTemperature) * (1 - exp(-dt / egtResponseSeconds))

  local thermostat = smoothRange(coolantTemperature, thermostatOpen, thermostatFull)
  local oilThermostat = smoothRange(oilTemperature, oilThermostatOpen, oilThermostatFull)

  if fanActive then
    if coolantTemperature <= fanOffTemperature then fanActive = false end
  elseif coolantTemperature >= fanOnTemperature then
    fanActive = true
  end

  local pump = idlePumpFraction + (1 - idlePumpFraction) * sqrt(clamp(rpmFraction, 0, 1))
  local speed = abs(physicsCar.speedKmh) / 3.6
  local coolingFlow = bridge.getCoolingFlow()
  local ramAir = (clamp(speed / referenceAirSpeed, 0, 1.5)) ^ ramAirExponent * bridge.getDensityRatio()
  local effectiveRamAir = ramAir * trafficAirflow
  local fanUA = fanActive and radiatorFanUA or 0
  -- A leading car blocks speed-dependent ram air. Natural convection and the
  -- radiator fan remain available, while grille tape still affects all paths.
  local radiatorUA = radiatorNaturalUA + thermostat * pump * coolingFlow * (radiatorMaxUA * effectiveRamAir + fanUA)
  local oilCoolerUA = oilThermostat * coolingFlow * oilCoolerMaxUA * effectiveRamAir

  -- Internal transfers use temperatures from the beginning of this fixed step,
  -- so each transfer is equal-and-opposite across its two thermal nodes.
  local qHeadCoolant = kHeadCoolant * (headTemperature - coolantTemperature)
  local qBlockCoolant = kBlockCoolant * (blockTemperature - coolantTemperature)
  local qHeadBlock = kHeadBlock * (headTemperature - blockTemperature)
  local qBlockOil = kBlockOil * (blockTemperature - oilTemperature)
  local qOilCoolant = kOilCoolant * (oilTemperature - coolantTemperature)
  local qExhaustHead = kExhaustHead * (exhaustTemperature - headTemperature)
  local qHeadAir = kHeadAir * (headTemperature - ambient)
  local qBlockAir = kBlockAir * (blockTemperature - ambient)
  local qRadiator = radiatorUA * (coolantTemperature - ambient)
  local qOilCooler = oilCoolerUA * (oilTemperature - ambient)

  headTemperature = headTemperature + (headInput + qExhaustHead - qHeadCoolant - qHeadBlock - qHeadAir) * dt / headCapacity
  blockTemperature = blockTemperature + (blockInput + qHeadBlock - qBlockCoolant - qBlockOil - qBlockAir) * dt / blockCapacity
  oilTemperature = oilTemperature + (oilInput + qBlockOil - qOilCoolant - qOilCooler) * dt / oilCapacity
  coolantTemperature = coolantTemperature + (qHeadCoolant + qBlockCoolant + qOilCoolant - qRadiator) * dt / coolantCapacity

  -- Keep bad configuration values from destabilizing AC while retaining a wide
  -- enough range for catastrophic-overheat behaviour.
  local floorTemperature = ambient - 2
  headTemperature = clamp(headTemperature, floorTemperature, 350)
  blockTemperature = clamp(blockTemperature, floorTemperature, 350)
  oilTemperature = clamp(oilTemperature, floorTemperature, 350)
  coolantTemperature = clamp(coolantTemperature, floorTemperature, 350)
  exhaustTemperature = clamp(exhaustTemperature, floorTemperature, 1200)

  -- Warning-to-critical range controls progressive torque derating only.
  local coolantDerate = severityAbove(coolantTemperature, coolantWarning, coolantCritical)
  local oilDerate = severityAbove(oilTemperature, oilWarning, oilCritical)
  local headDerate = severityAbove(headTemperature, headWarning, headCritical)
  local exhaustDerate = severityAbove(exhaustTemperature, egtWarning, egtCritical)
  overheatSeverity = coolantDerate
  derateSource = 'coolant'
  if oilDerate > overheatSeverity then overheatSeverity, derateSource = oilDerate, 'oil' end
  if headDerate > overheatSeverity then overheatSeverity, derateSource = headDerate, 'head' end
  if exhaustDerate > overheatSeverity then overheatSeverity, derateSource = exhaustDerate, 'egt' end
  if overheatSeverity <= 0 then derateSource = 'none' end

  -- Permanent hot damage starts at CRITICAL, not WARNING. It reaches the
  -- configured base rate at FAILURE and accelerates beyond it by exponent.
  local coolantDamage = severityAbove(coolantTemperature, coolantCritical, coolantFailure)
  local oilDamage = severityAbove(oilTemperature, oilCritical, oilFailure)
  local headDamage = severityAbove(headTemperature, headCritical, headFailure)
  local exhaustDamage = severityAbove(exhaustTemperature, egtCritical, egtFailure)
  damageSeverity = coolantDamage
  damageSource = 'coolant'
  if oilDamage > damageSeverity then damageSeverity, damageSource = oilDamage, 'oil' end
  if headDamage > damageSeverity then damageSeverity, damageSource = headDamage, 'head' end
  if exhaustDamage > damageSeverity then damageSeverity, damageSource = exhaustDamage, 'egt' end
  if damageSeverity <= 0 then damageSource = 'none' end

  local hotDamageRate = damageAtFailure * damageSeverity ^ damageExponent
  local coldDamagePerSecond = 0
  if oilTemperature < coldOilLimit and effectiveLoad > coldDamageLoad then
    coldDamagePerSecond = coldDamageRate * ((coldOilLimit - oilTemperature) / max(1, coldOilLimit - ambient)) * effectiveLoad
    if coldDamagePerSecond > hotDamageRate then damageSource = 'cold-oil' end
  end
  damageRate = hotDamageRate + coldDamagePerSecond

  -- AC's shared engine life remains authoritative, preserving over-rev and
  -- other mechanical failures. Do not compare it with a private previous-life
  -- value or rewrite it when this model has no thermal damage to apply.
  engineLife = clamp(physicsCar.engineLifeLeft, 0, 1000)
  if damageRate > 0 then
    engineLife = max(0, engineLife - damageRate * dt)
    ac.setEngineLifeLeft(engineLife)
  end

  local hotLoss = maxHotTorqueLoss * clamp(overheatSeverity, 0, 1)
  local coldLoss = maxColdTorqueLoss * clamp((coldDerateEnd - oilTemperature) / max(1, coldDerateEnd - ambient), 0, 1)
  torqueLoss = clamp(max(hotLoss, coldLoss), 0, 0.75)

  publishState()
  publishDebug(dt)
end

-- Preserve temperatures and accumulated thermal damage in CSP save states and
-- replays. This callback is outside the physics hot path.
ac.extendCarState('nascar.thermal.v1', function()
  return string.format('%.3f,%.3f,%.3f,%.3f,%.3f,%.3f',
    headTemperature, blockTemperature, coolantTemperature,
    oilTemperature, exhaustTemperature, engineLife)
end, function(data)
  if not data then return end
  local head, block, coolant, oil, exhaust, life = data:match(
    '^([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),([^,]+)$')
  head, block, coolant = tonumber(head), tonumber(block), tonumber(coolant)
  oil, exhaust, life = tonumber(oil), tonumber(exhaust), tonumber(life)
  if head and block and coolant and oil and exhaust and life then
    headTemperature, blockTemperature, coolantTemperature = head, block, coolant
    oilTemperature, exhaustTemperature, engineLife = oil, exhaust, life
    publishState()
  end
end)

function M.reset()
  resetThermalState()
  publishState()
end

function M.update(dt)
  ac.setWaterTemperature(safeGameWaterTemperature, true)

  local currentTorque = physicsCar.engineTorque
  if currentTorque > 0 and torqueLoss > 0 then
    ac.setExtraTorque(-currentTorque * torqueLoss / (1 - torqueLoss))
  else
    ac.setExtraTorque(0)
  end

  accumulator = accumulator + min(max(dt, 0), 0.25)
  local substeps = 0
  while accumulator >= modelDt and substeps < maxSubsteps do
    thermalStep(modelDt)
    accumulator = accumulator - modelDt
    substeps = substeps + 1
  end
  if substeps == maxSubsteps and accumulator >= modelDt then
    accumulator = accumulator % modelDt
  end
end

function M.connect(aeroBridge)
  bridge = aeroBridge
  M.reset()
  if debugEnabled then
    debugLog(format('thermal module connected car=%s index=%d model=%.1fHz',
      ac.getCarID(carIndex) or '?', carIndex, modelHz))
  end
  return M
end

if jit and jit.off then
  jit.off(resetThermalState, true)
  jit.off(scanTrafficAhead, true)
  jit.off(updateTrafficCooling, true)
  jit.off(publishState, true)
  jit.off(publishDebug, true)
  jit.off(thermalStep, true)
  jit.off(M.reset, true)
  jit.off(M.update, true)
end

return M

