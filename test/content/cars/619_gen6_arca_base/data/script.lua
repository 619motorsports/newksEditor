-- NASCAR aerodynamic coordinator. Thermal and weight systems live in separate modules.
local physicsCar = ac.accessCarPhysics()
local carIndex = car.index
local sim = ac.getSim()

local min, max, abs, exp = math.min, math.max, math.abs, math.exp
local floor, format = math.floor, string.format

local function clamp(value, lower, upper)
  return min(upper, max(lower, value))
end

local function smoothRange(value, startValue, endValue)
  local x = clamp((value - startValue) / (endValue - startValue), 0, 1)
  return x * x * (3 - 2 * x)
end

local settings = ac.INIConfig.carData(carIndex, 'nascar_thermal.ini')
local function setting(section, key, fallback)
  return settings:get(section, key, fallback)
end

local debugEnabled = setting('DEBUG', 'ENABLED', false)
local debugPeriod = max(0.25, setting('DEBUG', 'PERIOD_SECONDS', 2))
local debugLiveValues = setting('DEBUG', 'LUA_DEBUG_VALUES', true)
local debugLogAeroWings = setting('DEBUG', 'LOG_AERO_WINGS', false)
local logPrefix = '[NASCAR_AERO] '

local tapeSetupID = setting('GRILLE_TAPE', 'SETUP_ID', 'nascar_grille_tape')
local tapeDefaultPercent = clamp(setting('GRILLE_TAPE', 'DEFAULT_PERCENT', 0), 0, 100)
local maxDragReduction = clamp(setting('GRILLE_TAPE', 'MAX_DRAG_REDUCTION', 0.025), 0, 0.20)
local minimumCoolingFlow = clamp(setting('GRILLE_TAPE', 'MIN_COOLING_FLOW', 0.08), 0, 1)
local tapeFlowExponent = max(0.1, setting('GRILLE_TAPE', 'COOLING_FLOW_EXPONENT', 1.15))

local densityAeroEnabled = setting('AERODYNAMICS', 'ENABLED', true)
local dragDensityResponse = clamp(setting('AERODYNAMICS', 'DRAG_DENSITY_RESPONSE', 1), -4, 4)
local downforceDensityResponse = clamp(setting('AERODYNAMICS', 'DOWNFORCE_DENSITY_RESPONSE', 1), -4, 4)
local minimumAeroGain = clamp(setting('AERODYNAMICS', 'MIN_GAIN_MULTIPLIER', 0.25), 0, 1)
local maximumAeroGain = max(1, setting('AERODYNAMICS', 'MAX_GAIN_MULTIPLIER', 2))
local densityUpdateHz = clamp(setting('AERODYNAMICS', 'UPDATE_HZ', 10), 1, 50)
local densityUpdateInterval = 1 / densityUpdateHz
local densityChangeEpsilon = clamp(setting('AERODYNAMICS', 'RATIO_CHANGE_EPSILON', 0.005), 0.001, 0.1)

local bubbleEnabled = setting('AIR_BUBBLES', 'ENABLED', true)
local bubbleScanHz = clamp(setting('AIR_BUBBLES', 'SCAN_HZ', 20), 2, 50)
local bubbleScanInterval = 1 / bubbleScanHz
local bubbleVehicleLength = max(0, setting('AIR_BUBBLES', 'VEHICLE_LENGTH_M', 5))
local bubbleRange = max(0.2, setting('AIR_BUBBLES', 'RANGE_M', 2.0))
local bubbleMinimumGap = clamp(setting('AIR_BUBBLES', 'MINIMUM_GAP_M', 0.30), 0, bubbleRange - 0.05)
local bubbleResetGap = max(bubbleRange + 0.1, setting('AIR_BUBBLES', 'RESET_GAP_M', 3.0))
local bubbleMaxLateral = max(0.1, setting('AIR_BUBBLES', 'MAX_LATERAL_M', 1.35))
local bubbleMaxVertical = max(0.1, setting('AIR_BUBBLES', 'MAX_VERTICAL_M', 2.0))
local bubbleMinDirectionDot = clamp(setting('AIR_BUBBLES', 'MIN_DIRECTION_DOT', 0.90), -1, 0.999)
local bubbleMinSpeedKmh = max(0, setting('AIR_BUBBLES', 'MIN_SPEED_KMH', 100))
local bubbleFrontBaseForce = max(0, setting('AIR_BUBBLES', 'FRONT_CUSHION_FORCE_N', 1800))
local bubbleRearPushForce = max(0, setting('AIR_BUBBLES', 'REAR_PUSH_FORCE_N', 700))
local bubbleClosingDamping = max(0, setting('AIR_BUBBLES', 'CLOSING_DAMPING_N_PER_MPS', 180))
local bubbleMaximumForce = max(0, setting('AIR_BUBBLES', 'MAX_FORCE_N', 3500))
local bubbleForceExponent = max(0.1, setting('AIR_BUBBLES', 'FORCE_EXPONENT', 1.5))
local bubbleBreakGap = clamp(setting('AIR_BUBBLES', 'BREAK_GAP_M', 0.12), 0, bubbleMinimumGap)
local bubbleBreakClosingSpeed = max(0, setting('AIR_BUBBLES', 'BREAK_CLOSING_SPEED_MPS', 7.0))
local bubbleResponseSeconds = max(0.005, setting('AIR_BUBBLES', 'RESPONSE_SECONDS', 0.06))

settings = nil
setting = nil

local function debugLog(message)
  if debugEnabled then ac.log(logPrefix .. message) end
end

if debugEnabled then ac.setLogSilent(false) end

local tapeSetup = ac.getScriptSetupValue(tapeSetupID)
local wingIndices, wingCdGains, wingClGains = {}, {}, {}
local wingCount = 0
local aero = ac.INIConfig.carData(carIndex, 'aero.ini')
for fallbackIndex, section in aero:iterate('WING') do
  local cdGain = aero:get(section, 'CD_GAIN', ac.INIConfig.OptionalNumber)
  if cdGain ~= nil then
    wingCount = wingCount + 1
    wingIndices[wingCount] = tonumber(section:match('_(%d+)$')) or fallbackIndex
    wingCdGains[wingCount] = cdGain
    wingClGains[wingCount] = aero:get(section, 'CL_GAIN', 1)
    if debugEnabled and debugLogAeroWings then
      debugLog(format('wing=%d section=%s baseCD=%.5f baseCL=%.5f',
        wingIndices[wingCount], section, wingCdGains[wingCount], wingClGains[wingCount]))
    end
  end
end
aero = nil

local tapeFraction, coolingFlow, tapeCdMultiplier = -1, 1, 1
local densityRatio, appliedDensityRatio = 1, -1
local dragMultiplier, downforceMultiplier = 1, 1
local densityAccumulator = densityUpdateInterval

local function applyWingGains()
  for i = 1, wingCount do
    ac.setWingGain(wingIndices[i],
      wingCdGains[i] * tapeCdMultiplier * dragMultiplier,
      wingClGains[i] * downforceMultiplier)
  end
end

local function sampleDensityRatio(dt, forceUpdate)
  if not forceUpdate then
    densityAccumulator = densityAccumulator + dt
    if densityAccumulator < densityUpdateInterval then return end
    densityAccumulator = densityAccumulator % densityUpdateInterval
  end

  local pressurePa = ac.getAirPressure(physicsCar.position) * 1000
  local temperatureK = physicsCar.ambientTemperature + 273.15
  local ambientDensity = pressurePa / (temperatureK * 287.058)
  local nextRatio = min(floor(physicsCar.airDensity / ambientDensity * 1000 + 0.5) / 1000, 1)
  if nextRatio ~= nextRatio or nextRatio < 0 then nextRatio = 0 end
  densityRatio = nextRatio
  if not forceUpdate and abs(nextRatio - appliedDensityRatio) < densityChangeEpsilon then return end
  appliedDensityRatio = nextRatio

  if densityAeroEnabled then
    local loss = 1 - nextRatio
    dragMultiplier = clamp(1 - loss * dragDensityResponse, minimumAeroGain, maximumAeroGain)
    downforceMultiplier = clamp(1 - loss * downforceDensityResponse, minimumAeroGain, maximumAeroGain)
  else
    dragMultiplier, downforceMultiplier = 1, 1
  end
  applyWingGains()
end

local function updateGrilleTape(forceUpdate)
  local percent = tapeSetup and tapeSetup.value or tapeDefaultPercent
  local nextTape = clamp(percent * 0.01, 0, 1)
  if not forceUpdate and abs(nextTape - tapeFraction) < 0.0001 then return end
  tapeFraction = nextTape
  coolingFlow = minimumCoolingFlow + (1 - minimumCoolingFlow) * (1 - tapeFraction) ^ tapeFlowExponent
  tapeCdMultiplier = 1 - maxDragReduction * tapeFraction
  applyWingGains()
end

local bubbleFrontIndex, bubbleRearIndex = -1, -1
local bubbleFrontGap, bubbleRearGap = -1, -1
local bubbleFrontClosing, bubbleRearClosing = 0, 0
local bubbleFrontBroken, bubbleRearBroken = false, false
local bubbleForceTarget, bubbleForceCurrent = 0, 0
local bubbleScanAccumulator = bubbleScanInterval
local bubblePoint = vec3(0, 0, 0)
local bubbleForce = vec3(0, 0, 0)

local function bubbleStrength(gap)
  if gap >= bubbleRange then return 0 end
  return clamp((bubbleRange - gap) / (bubbleRange - bubbleMinimumGap), 0, 1) ^ bubbleForceExponent
end

local function updateBroken(previousIndex, nextIndex, broken, gap, closingSpeed)
  if nextIndex < 0 or nextIndex ~= previousIndex then broken = false end
  if nextIndex >= 0 and not broken and
      (gap <= bubbleBreakGap or closingSpeed >= bubbleBreakClosingSpeed) then
    broken = true
  end
  return broken
end

local function scanAirBubbles()
  local previousFront, previousRear = bubbleFrontIndex, bubbleRearIndex
  local bestFrontLongitudinal, bestRearLongitudinal = math.huge, math.huge
  local nextFront, nextRear = -1, -1
  local nextFrontGap, nextRearGap = -1, -1
  local nextFrontClosing, nextRearClosing = 0, 0

  if bubbleEnabled and abs(physicsCar.speedKmh) >= bubbleMinSpeedKmh then
    local position, look, side, up = physicsCar.position, physicsCar.look, physicsCar.side, physicsCar.up
    local ownVelocity = physicsCar.velocity
    local ownForwardSpeed = ownVelocity.x * look.x + ownVelocity.y * look.y + ownVelocity.z * look.z
    for i = 0, sim.carsCount - 1 do
      if i ~= carIndex then
        local other = ac.getCar(i)
        if other and other.isActive and other.isConnected then
          local p = other.position
          local dx, dy, dz = p.x - position.x, p.y - position.y, p.z - position.z
          local longitudinal = dx * look.x + dy * look.y + dz * look.z
          local distance = abs(longitudinal)
          if distance > 0 and distance <= bubbleVehicleLength + bubbleResetGap then
            local lateral = abs(dx * side.x + dy * side.y + dz * side.z)
            local vertical = abs(dx * up.x + dy * up.y + dz * up.z)
            local otherLook = other.look
            local directionDot = otherLook.x * look.x + otherLook.y * look.y + otherLook.z * look.z
            if lateral <= bubbleMaxLateral and vertical <= bubbleMaxVertical and
                directionDot >= bubbleMinDirectionDot then
              local gap = max(0, distance - bubbleVehicleLength)
              local velocity = other.velocity
              local otherForwardSpeed = velocity.x * look.x + velocity.y * look.y + velocity.z * look.z
              if longitudinal > 0 and distance < bestFrontLongitudinal then
                bestFrontLongitudinal, nextFront, nextFrontGap = distance, i, gap
                nextFrontClosing = ownForwardSpeed - otherForwardSpeed
              elseif longitudinal < 0 and distance < bestRearLongitudinal then
                bestRearLongitudinal, nextRear, nextRearGap = distance, i, gap
                nextRearClosing = otherForwardSpeed - ownForwardSpeed
              end
            end
          end
        end
      end
    end
  end

  bubbleFrontBroken = updateBroken(previousFront, nextFront, bubbleFrontBroken,
    nextFrontGap, nextFrontClosing)
  bubbleRearBroken = updateBroken(previousRear, nextRear, bubbleRearBroken,
    nextRearGap, nextRearClosing)
  bubbleFrontIndex, bubbleRearIndex = nextFront, nextRear
  bubbleFrontGap, bubbleRearGap = nextFrontGap, nextRearGap
  bubbleFrontClosing, bubbleRearClosing = nextFrontClosing, nextRearClosing

  local target = 0
  if nextFront >= 0 and not bubbleFrontBroken then
    local strength = bubbleStrength(nextFrontGap)
    local force = (bubbleFrontBaseForce + bubbleClosingDamping * max(0, nextFrontClosing)) * strength
    target = target - min(bubbleMaximumForce, force)
  end
  if nextRear >= 0 and not bubbleRearBroken then
    local strength = bubbleStrength(nextRearGap)
    local force = (bubbleRearPushForce + bubbleClosingDamping * 0.35 * max(0, nextRearClosing)) * strength
    target = target + min(bubbleMaximumForce, force)
  end
  bubbleForceTarget = target
end

local function updateAirBubbles(dt)
  bubbleScanAccumulator = bubbleScanAccumulator + dt
  if bubbleScanAccumulator >= bubbleScanInterval then
    bubbleScanAccumulator = bubbleScanAccumulator % bubbleScanInterval
    scanAirBubbles()
  end
  local response = 1 - exp(-max(0, dt) / bubbleResponseSeconds)
  bubbleForceCurrent = bubbleForceCurrent + (bubbleForceTarget - bubbleForceCurrent) * response
  if abs(bubbleForceCurrent) > 0.5 then
    bubbleForce.z = bubbleForceCurrent
    ac.addForce(bubblePoint, true, bubbleForce, true)
  end
end

local aeroDebugAccumulator = 0
local function publishAeroDebug(dt)
  if not debugEnabled then return end
  aeroDebugAccumulator = aeroDebugAccumulator + dt
  if aeroDebugAccumulator < debugPeriod then return end
  aeroDebugAccumulator = aeroDebugAccumulator % debugPeriod
  debugLog(format('tape=%.1f%% density=%.3f drag=%.3f downforce=%.3f bubbleForce=%.1fN front=%d gap=%.2f closing=%.2f broken=%s rear=%d gap=%.2f closing=%.2f broken=%s',
    tapeFraction * 100, densityRatio, dragMultiplier, downforceMultiplier,
    bubbleForceCurrent, bubbleFrontIndex, bubbleFrontGap, bubbleFrontClosing,
    tostring(bubbleFrontBroken), bubbleRearIndex, bubbleRearGap, bubbleRearClosing,
    tostring(bubbleRearBroken)))
  if debugLiveValues then
    ac.debug('NASCAR Aero/Density ratio', densityRatio)
    ac.debug('NASCAR Aero/Drag multiplier', dragMultiplier)
    ac.debug('NASCAR Aero/Downforce multiplier', downforceMultiplier)
    ac.debug('NASCAR Bubble/Force N', bubbleForceCurrent)
    ac.debug('NASCAR Bubble/Front gap m', bubbleFrontGap)
    ac.debug('NASCAR Bubble/Front closing mps', bubbleFrontClosing)
    ac.debug('NASCAR Bubble/Front broken', bubbleFrontBroken and 1 or 0)
    ac.debug('NASCAR Bubble/Rear gap m', bubbleRearGap)
    ac.debug('NASCAR Bubble/Rear broken', bubbleRearBroken and 1 or 0)
  end
end

local function resetAerodynamics()
  tapeFraction, coolingFlow, tapeCdMultiplier = -1, 1, 1
  densityRatio, appliedDensityRatio = 1, -1
  dragMultiplier, downforceMultiplier = 1, 1
  densityAccumulator = densityUpdateInterval
  bubbleFrontIndex, bubbleRearIndex = -1, -1
  bubbleFrontGap, bubbleRearGap = -1, -1
  bubbleFrontClosing, bubbleRearClosing = 0, 0
  bubbleFrontBroken, bubbleRearBroken = false, false
  bubbleForceTarget, bubbleForceCurrent = 0, 0
  bubbleScanAccumulator = bubbleScanInterval
  aeroDebugAccumulator = 0
  sampleDensityRatio(0, true)
  updateGrilleTape(true)
end

if jit and jit.off then
  jit.off(sampleDensityRatio, true)
  jit.off(scanAirBubbles, true)
end

resetAerodynamics()

-- Only the primary, locally driven car should run the custom engine model.
-- Remote online cars can still be user-controlled at their own clients, so
-- isUserControlled alone is not enough to identify the local player here.
local thermalEnabled = carIndex == 0 and car.isUserControlled and
  not car.isAIControlled and not car.isRemote
local thermal = nil
if thermalEnabled then
  thermal = require(package.relative('nascar_thermal'))
  thermal.connect({
    getTapeFraction = function() return tapeFraction end,
    getCoolingFlow = function() return coolingFlow end,
    getDensityRatio = function() return densityRatio end
  })
end
local weightControls = require(package.relative('nascar_weights'))
weightControls.connect()

function script.reset()
  resetAerodynamics()
  if thermal then thermal.reset() end
  weightControls.reset()
end

function script.update(dt)
  updateGrilleTape(false)
  sampleDensityRatio(dt, false)
  updateAirBubbles(dt)
  if thermal then thermal.update(dt) end
  weightControls.update()
  publishAeroDebug(dt)
end

debugLog(format('loaded car=%s index=%d wings=%d density=%.1fHz bubble=%s scan=%.1fHz thermal=%s',
  ac.getCarID(carIndex) or '?', carIndex, wingCount, densityUpdateHz,
  tostring(bubbleEnabled), bubbleScanHz, tostring(thermalEnabled)))
