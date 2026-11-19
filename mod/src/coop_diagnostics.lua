-- One bounded diagnostic pass. Observe calls/controllers; never choose, wait
-- for, stop, or replay presentation here. Native log prefixes supply wall time.
if not (COOP_TestMode and COOP_TestMode()) or COOP_DiagnosticsInstalled then return end

local unpackValues = unpack or table.unpack
local guest = COOP_IsGuest and COOP_IsGuest()
local run = tostring({})
local main = {}
local contexts = setmetatable({}, {__mode='k'})
local wrapped = setmetatable({}, {__mode='k'})
local watch = {}
local watchIndex = 1
local frames, records, serial = 0, 0, 0
local hand, showdownHand = '-', nil
local recordLimit = 4000

local function token(value)
  local s = tostring(value)
  if #s > 140 then s = s:sub(1,140) .. '~truncated' end
  return (s:gsub('[^%w%._%-%:/]', function(c) return string.format('%%%02X',string.byte(c)) end))
end
local function context()
  local thread = coroutine.running() or main
  if not contexts[thread] then contexts[thread] = {stack={}} end
  return contexts[thread], tostring(thread)
end
local function emit(event, fields)
  if records >= recordLimit then return end
  records = records + 1
  local ctx, thread = context()
  local parts = {'COOP_DIAG v=1', 'event='..event, 'run='..token(run),
                 'role='..(guest and 'guest' or 'host'), 'hand='..token(hand),
                 'street='..token(Logic and Logic.iPokerState),
                 'instant='..token(Logic and Logic.bModeInstant),
                 'thread='..token(thread), 'parent='..token(ctx.stack[#ctx.stack] or 0)}
  for _, pair in ipairs(fields or {}) do parts[#parts+1] = pair[1]..'='..token(pair[2]) end
  Print(table.concat(parts,' '))
  if records == recordLimit then Print('COOP_DIAG v=1 event=limit run='..token(run)..' records='..records) end
end
local function gameplay() return Logic and Logic.bInGame == true end
local function showdown()
  return gameplay() and (showdownHand ~= nil or (GameObject and GameObject.runningDealChoreWithChord == true))
end
local function pack(...) return {n=select('#',...), ...} end
local function addWatch(key, controller)
  if not gameplay() or not controller then return end
  if #watch >= 256 then emit('watch_limit'); return end
  watch[#watch+1] = {key=key, controller=controller, frame=frames, seen=false}
  emit('controller',{{'phase','registered'},{'key',key}})
end
COOP_DiagWatchController = addWatch
COOP_DiagRawExchange = function(phase, dlg, node, instance, count, ready, key, matched)
  -- Intentionally BEFORE replay readiness/identity/sent-key filters, including
  -- end callbacks arriving after gameplay/connection readiness changes.
  emit('raw_exchange',{{'phase',phase},{'dlg',dlg},{'node',node},
    {'instance',instance},{'count',count},{'node_type',type(node)},
    {'instance_type',type(instance)},{'count_type',type(count)},
    {'ready_cached',ready},{'key',key},{'matched',matched},{'gameplay',gameplay()}})
end
COOP_DiagExchangeDecision = function(phase, reason, key)
  emit('exchange_filter',{{'phase',phase},{'reason',reason},{'key',key}})
end

-- Pure Lua forwarding is required: original calls may Yield under Lua 5.1.
-- No pcall around originals; errors retain their existing behavior and an
-- unmatched enter is reported as incomplete rather than fabricated completion.
local function wrap(owner, key, label, predicate, describe)
  if type(owner) ~= 'table' or type(owner[key]) ~= 'function' then return end
  wrapped[owner] = wrapped[owner] or {}
  if wrapped[owner][key] then return end
  local original = owner[key]
  owner[key] = function(...)
    if not gameplay() or (predicate and not predicate(...)) or records >= recordLimit then
      return original(...)
    end
    local arguments = pack(...)
    local self = arguments[1]
    if label:match('^GameTable%.') and type(self) == 'table' then hand = self.handNumber or hand end
    if label == 'GameTable.InitRound' then showdownHand = nil end
    if label == 'GameTable.ShowCards' then showdownHand = hand end
    serial = serial + 1
    local id = serial
    local ctx = context()
    local fields = {{'phase','enter'},{'call',id},{'op',label},
                    {'paused',Logic and Logic.bPausedPokerActions}}
    if describe then
      for _,pair in ipairs(describe(unpackValues(arguments,1,arguments.n))) do fields[#fields+1]=pair end
    end
    emit('boundary',fields)
    ctx.stack[#ctx.stack+1] = id
    local result = pack(original(unpackValues(arguments,1,arguments.n)))
    ctx.stack[#ctx.stack] = nil
    emit('boundary',{{'phase','exit'},{'call',id},{'op',label},
                     {'returns',result.n},{'result1',result[1]}})
    if label == 'ChorePlay' and result[1] then addWatch('script:'..id,result[1]) end
    return unpackValues(result,1,result.n)
  end
  wrapped[owner][key] = owner[key]
  emit('capability',{{'api',label},{'available',true}})
end
local function pairArgs(a,b) return {{'arg1',a},{'arg2',b}} end
local function methodArgs(self,a,b)
  return {{'arg1',type(a)=='table' and (a.name or a.index or tostring(a)) or a},{'arg2',b}}
end
local function installTargets()
  for _,name in ipairs({'InitRound','DoRoundState','ResolvePot','EndRound'}) do
    wrap(GameTable,name,'GameTable.'..name,nil,methodArgs)
  end
  wrap(GameTable,'RunAllInBeforeRiverShowCardsChore','GameTable.ShowCards',nil,methodArgs)
  for _,name in ipairs({'PokerAction','PlayPokerLine','PlayPokerResponse','PlayLine'}) do
    wrap(Dialogue,name,'Dialogue.'..name,nil,methodArgs)
  end
  for _,name in ipairs({'DlgRun','DlgRunAndWait','DlgWait'}) do wrap(_G,name,name,nil,pairArgs) end
  wrap(_G,'ChorePlayAndWait','ChorePlayAndWait',showdown,pairArgs)
  wrap(_G,'ChorePlay','ChorePlay',function(resource)
    return showdown() or (type(resource)=='string' and resource:match('^dealer_deal'))
  end,pairArgs)
  wrap(_G,'ChorePlayOnAgent','ChorePlayOnAgent',showdown,pairArgs)
  wrap(_G,'CameraActivate','CameraActivate',showdown,pairArgs)
  wrap(_G,'DlgSetExchangeChoreAgentRemap','DlgSetExchangeChoreAgentRemap',showdown,pairArgs)
  wrap(_G,'ShowdownResponseManager_Update','Showdown.Update',nil,pairArgs)
  wrap(ModeFlop,'Deal','ModeFlop.Deal',nil,methodArgs)
  wrap(PostFlopDealing,'Deal','PostFlopDealing.Deal',nil,methodArgs)
end

-- Observe all dialogue node events during showdown, in addition to exchange
-- events. Non-exchange IDs reveal graph work absent from our leaf-only stream;
-- no unverified node-type API is called or guessed here.
for _,spec in ipairs({{'Callback_DialogueNodeBegin','begin'},{'Callback_DialogueNodeEnd','end'}}) do
  local callback, phase = _G[spec[1]], spec[2]
  local available = callback and type(callback.Add)=='function'
  emit('capability',{{'api',spec[1]},{'available',available or false}})
  if available then callback:Add(function(dlg,node,instance,count)
    if showdown() or (gameplay() and tostring(dlg):lower():find('showdown',1,true)) then
      emit('graph_node',{{'phase',phase},{'dlg',dlg},{'node',node},{'instance',instance},{'count',count}})
    end
  end) end
end
for _,name in ipairs({'ControllerGetTime','ControllerGetLength','ControllerIsPlaying'}) do
  emit('capability',{{'api',name},{'available',type(_G[name])=='function'}})
end

local function pollControllers()
  local budget = math.min(8,#watch)
  for i=1,budget do
    if watchIndex > #watch then watchIndex=1 end
    local item = watch[watchIndex]
    if not item then break end
    local ok,playing = pcall(ControllerIsPlaying,item.controller)
    local expired = frames-item.frame >= 3600
    if not ok or not playing or expired then
      local fields={{'phase',not ok and 'error' or expired and 'timeout' or 'stopped'},
                    {'key',item.key},{'seen_playing',item.seen},{'age_frames',frames-item.frame}}
      for _,name in ipairs({'ControllerGetTime','ControllerGetLength'}) do
        if type(_G[name])=='function' then
          local read,value = pcall(_G[name],item.controller)
          fields[#fields+1]={name,read and value or 'unavailable'}
        end
      end
      emit('controller',fields)
      watch[watchIndex]=watch[#watch]; watch[#watch]=nil
    else
      if not item.seen then emit('controller',{{'phase','playing'},{'key',item.key}}); item.seen=true end
      watchIndex=watchIndex+1
    end
  end
end

installTargets()
ThreadStart(function()
  while true do
    frames=frames+1
    if records < recordLimit then
      if frames % 30 == 0 then installTargets() end
      if gameplay() then pollControllers()
      elseif #watch>0 then
        emit('watch_clear',{{'reason','left_gameplay'},{'count',#watch}})
        watch={}; watchIndex=1; showdownHand=nil
      end
    end
    Yield()
  end
end)
COOP_DiagnosticsInstalled = true
emit('installed',{{'version',1},{'record_limit',recordLimit},{'poll_budget',8}})
