-- NASCAR localized ballast controls. Loaded by data/script.lua.
local M = {}

local carIndex = car.index
local min, max, abs = math.min, math.max, math.abs
local format = string.format

local function clamp(value, lower, upper)
  return min(upper, max(lower, value))
end

local settings = ac.INIConfig.carData(carIndex, 'nascar_weights.ini')
local function setting(section, key, fallback)
  return settings:get(section, key, fallback)
end

local debugEnabled = setting('DEBUG', 'ENABLED', false)
local debugLiveValues = setting('DEBUG', 'LUA_DEBUG_VALUES', true)
local logPrefix = '[NASCAR_WEIGHTS] '

local maximumMass = max(0, setting('MODEL', 'MAX_MASS_PER_CONTROL_KG', 100))
local linkageDamping = max(0, setting('MODEL', 'LINKAGE_DAMPING', 7000000))
local linkageStiffness = max(0, setting('MODEL', 'LINKAGE_STIFFNESS', 900000000))
local changeEpsilon = max(0.0001, setting('MODEL', 'CHANGE_EPSILON_KG', 0.001))
local totalBallastMass = max(0, setting('BALLAST_POOL', 'TOTAL_KG', 150))

local function readControl(section, defaultID, key)
  local position = vec3(
    setting(section, 'POS_X', 0),
    setting(section, 'POS_Y', 0),
    setting(section, 'POS_Z', 0))
  local massBox = vec3(
    max(0.01, setting(section, 'BOX_X', 0.20)),
    max(0.01, setting(section, 'BOX_Y', 0.20)),
    max(0.01, setting(section, 'BOX_Z', 0.20)))
  local setupID = setting(section, 'SETUP_ID', defaultID)
  return {
    name = section,
    enabled = setting(section, 'ENABLED', true),
    setup = ac.getScriptSetupValue(setupID),
    setupID = setupID,
    defaultMass = clamp(setting(section, 'DEFAULT_KG', 0), 0, maximumMass),
    position = position,
    massBox = massBox,
    key = key,
    targetMass = 0,
    appliedMass = -1
  }
end

-- Non-zero keys allow each mass to be updated in place instead of creating a
-- new extra-mass object when its setup value changes.
local controls = {
  readControl('WEDGE', 'nascar_wedge_weight', 1),
  readControl('LEFT_SIDE_BIAS', 'nascar_left_side_weight', 2),
  readControl('NOSE_WEIGHT', 'nascar_nose_weight', 3)
}

-- The pool is the part of the fixed ballast budget that has not been moved to
-- one of the setup-controlled locations. Keeping it as a fourth keyed mass
-- makes every setup use exactly TOTAL_KG of ballast.
local ballastPool = {
  name = 'BALLAST_POOL',
  position = vec3(
    setting('BALLAST_POOL', 'POS_X', 0),
    setting('BALLAST_POOL', 'POS_Y', 0),
    setting('BALLAST_POOL', 'POS_Z', 0)),
  massBox = vec3(
    max(0.01, setting('BALLAST_POOL', 'BOX_X', 0.20)),
    max(0.01, setting('BALLAST_POOL', 'BOX_Y', 0.20)),
    max(0.01, setting('BALLAST_POOL', 'BOX_Z', 0.20))),
  key = 4,
  targetMass = totalBallastMass,
  appliedMass = -1
}

settings = nil
setting = nil

local function debugLog(message)
  if debugEnabled then ac.log(logPrefix .. message) end
end

if debugEnabled then ac.setLogSilent(false) end

local function selectedMass(control)
  if not control.enabled then return 0 end
  local value = control.setup and control.setup.value or control.defaultMass
  if value ~= value then return 0 end
  return clamp(value, 0, maximumMass)
end

local function applyMass(control, mass)
  if abs(mass - control.appliedMass) < changeEpsilon then return end
  control.appliedMass = mass
  ac.setExtraMass(control.position, mass, control.massBox,
    linkageDamping, linkageStiffness, control.key)

  if debugEnabled then
    debugLog(format('%s=%.3fkg position=(%.3f, %.3f, %.3f)',
      control.name, mass, control.position.x, control.position.y,
      control.position.z))
  end
end

local function calculateTargets()
  local requestedTotal = 0
  for i = 1, #controls do
    local mass = selectedMass(controls[i])
    controls[i].targetMass = mass
    requestedTotal = requestedTotal + mass
  end

  -- A hand-edited setup or mismatched per-car configuration must not be able
  -- to allocate more mass than the fixed pool. Preserve the requested weight
  -- distribution while constraining its sum to the available ballast.
  local allocationScale = 1
  if requestedTotal > totalBallastMass and requestedTotal > 0 then
    allocationScale = totalBallastMass / requestedTotal
  end

  local localizedTotal = 0
  for i = 1, #controls do
    local mass = controls[i].targetMass * allocationScale
    controls[i].targetMass = mass
    localizedTotal = localizedTotal + mass
  end
  ballastPool.targetMass = max(0, totalBallastMass - localizedTotal)
  return requestedTotal, localizedTotal, allocationScale
end

local function publishDebug()
  if not debugEnabled or not debugLiveValues then return end
  ac.debug('NASCAR Weights/Wedge kg', controls[1].appliedMass)
  ac.debug('NASCAR Weights/Left-side kg', controls[2].appliedMass)
  ac.debug('NASCAR Weights/Nose kg', controls[3].appliedMass)
  ac.debug('NASCAR Weights/Reserve kg', ballastPool.appliedMass)
  ac.debug('NASCAR Weights/Total kg', ballastPool.appliedMass +
    controls[1].appliedMass + controls[2].appliedMass + controls[3].appliedMass)
end

function M.reset()
  -- CSP calls script.reset() while the native car and its ODE bodies are being
  -- rebuilt. ac.setExtraMass() can create an ODE-linked body, so calling it
  -- from that callback can cross into a half-reset native state and throw a
  -- C++ exception. Invalidate our cache here; the next regular physics update
  -- will safely recreate or update each keyed mass, including the reserve.
  ballastPool.appliedMass = -1
  for i = 1, #controls do
    controls[i].appliedMass = -1
  end
end

function M.update()
  local requestedTotal, localizedTotal, allocationScale = calculateTargets()
  local changed = false

  -- Move mass out of the reserve before adding it at localized positions. All
  -- writes happen in one physics callback and their final sum is TOTAL_KG.
  local previousPool = ballastPool.appliedMass
  applyMass(ballastPool, ballastPool.targetMass)
  if ballastPool.appliedMass ~= previousPool then changed = true end
  for i = 1, #controls do
    local previous = controls[i].appliedMass
    applyMass(controls[i], controls[i].targetMass)
    if controls[i].appliedMass ~= previous then changed = true end
  end
  if changed then
    if debugEnabled then
      debugLog(format('allocation requested=%.3fkg localized=%.3fkg reserve=%.3fkg total=%.3fkg scale=%.5f',
        requestedTotal, localizedTotal, ballastPool.appliedMass,
        ballastPool.appliedMass + localizedTotal, allocationScale))
    end
    publishDebug()
  end
end

function M.connect()
  M.reset()
  if debugEnabled then
    debugLog(format('weight module connected car=%s index=%d total=%.1fkg maximumPerControl=%.1fkg',
      ac.getCarID(carIndex) or '?', carIndex, totalBallastMass, maximumMass))
  end
  return M
end

if jit and jit.off then
  jit.off(applyMass, true)
  jit.off(calculateTargets, true)
  jit.off(M.reset, true)
  jit.off(M.update, true)
end

return M
