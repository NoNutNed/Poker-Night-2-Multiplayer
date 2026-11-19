-- Opt-in proof of selected-exchange voice/animation replay. No menu automation.
-- This runs after the existing wrapper; all waiting stays in a Lua coroutine.
if not (COOP_TestMode and COOP_TestMode()) or COOP_ExchangeInstalled then return end
assert(Callback_DialogueExchangeNodeBegin and Callback_DialogueExchangeNodeEnd,
       'exchange callbacks not loaded yet')
assert(type(ThreadStart) == 'function' and type(Yield) == 'function', 'exchange threads unavailable')
assert(type(DlgExchangeGetChore) == 'function' and type(ChorePlay) == 'function', 'exchange playback unavailable')
assert(type(ControllerIsPlaying) == 'function' and type(ControllerKill) == 'function', 'exchange controllers unavailable')
assert(Dialogue and Dialogue.StartBanter and Dialogue.UpdateBanter, 'dialogue not loaded yet')
assert(type(DlgRun) == 'function' and type(DlgRunAndWait) == 'function' and COOP_DlgSilenced,
       'dialogue ownership wrapper not installed yet')
local voiceMode=COOP_VoiceOptions and COOP_VoiceOptions()==1
if voiceMode then
  for _,name in ipairs({'PlayPokerResponse','PlayPokerLine','PlayLine','PokerAction'}) do
    assert(type(Dialogue[name])=='function','voice method unavailable: '..name)
  end
  for _,name in ipairs({'COOP_VoiceOpen','COOP_VoiceEnd','COOP_VoiceStatus','COOP_VoiceState','COOP_VoiceDone','COOP_VoiceCancel','COOP_VoiceTakeCancel','DlgStop'}) do
    assert(type(_G[name])=='function','voice API unavailable: '..name)
  end
end

local guest = COOP_IsGuest and COOP_IsGuest()
local controllers = {}
local naturalFinish = {}
local finishing
local voiceScopes,voiceKeys,cancelledKeys={},{},{}
local sent = {}
local executions = {}
local completed = {}
local activeEpoch = 0
local ready = false
local play = ChorePlay

local function trace(s) Print('COOP_EXCHANGE ' .. s) end
local function hasAuthoredExit(dlg,node,chore)
  if dlg=='dealer.dlog' then
    -- Shipped dealer_blinds graph references #2 and #4 through #35.
    -- Each of these finite chores contains a nested GLaDOS departure.
    local variant=tonumber(chore:match('^dealer_dealer_deal_(%d+)%.chore$'))
    return variant==2 or (variant and variant>=4 and variant<=35)
  end
  return dlg=='responses.dlog' and node=='14703132049068486156' and chore=='responses_check_42.chore'
end
local function voiceFail(reason)
  trace('voice_fault '..reason);COOP_ExchangeAbort();error('Co-op voice stopped: '..reason)
end
local function stopController(controller)
  local ok, playing = pcall(ControllerIsPlaying, controller)
  if ok and playing then ok = pcall(ControllerKill, controller) end
  if not ok then trace('cleanup_error') end
end
local function stopAll()
  for key, controller in pairs(controllers) do
    stopController(controller)
    controllers[key] = nil
  end
  naturalFinish = {}
  finishing = nil
  voiceKeys={};cancelledKeys={}
end
local function recordEnd(event)
  completed[event.key]=true
  trace('end epoch=' .. tostring(event.epoch) .. ' seq=' .. tostring(event.seq) .. ' key=' .. event.key)
end
local function finishEnd(event)
  if voiceKeys[event.key] and not cancelledKeys[event.key] and COOP_VoiceDone(event.key)~=1 then voiceFail('completion_send_failed') end
  controllers[event.key]=nil;naturalFinish[event.key]=nil;voiceKeys[event.key]=nil;cancelledKeys[event.key]=nil
  recordEnd(event)
end
local function pollVoiceCancels()
  if not voiceMode then return end
  for i=1,64 do
    local key=COOP_VoiceTakeCancel();if not key then break end
    if not completed[key] then cancelledKeys[key]=true end
    if controllers[key] then stopController(controllers[key]);controllers[key]=nil end
    naturalFinish[key]=nil;voiceKeys[key]=nil
    if finishing and finishing.key==key then
      recordEnd(finishing);finishing=nil;cancelledKeys[key]=nil
    end
    trace('voice_cancel key='..key)
  end
end
local function updateReady()
  local gameplay = Logic and Logic.bInGame == true and COOP_PacingInstalled == true and COOP_ShowdownInstalled == true and COOP_BustoutInstalled == true
  if COOP_ControlOptions and COOP_ControlOptions()==1 and
     (not COOP_ControlInstalled or not COOP_CardUIInstalled) then gameplay=false end
  local enabled, epoch = COOP_ExchangeReady(gameplay and '1' or '0')
  local nowReady = enabled == 1 and gameplay
  if epoch ~= activeEpoch or not nowReady then
    stopAll()
    sent = {}
    executions = {}
    completed = {}
    if (epoch ~= activeEpoch or ready) and COOP_PaceReset then COOP_PaceReset() end
    if (epoch ~= activeEpoch or ready) and COOP_ShowdownReset then COOP_ShowdownReset() end
    if (epoch ~= activeEpoch or ready) and COOP_BustoutReset then COOP_BustoutReset(nowReady and gameplay) end
    activeEpoch = epoch
  end
  if nowReady ~= ready then trace(nowReady and ('ready epoch=' .. tostring(epoch)) or 'waiting_or_stopped') end
  ready = nowReady
  return ready, activeEpoch
end
COOP_ExchangeSync = updateReady
local function foregroundAllowed()
  return voiceMode and Logic and Logic.bInGame==true and not Logic.bModeInstant and not Logic.bBanterPlaying and
    not (GameObject and (GameObject.runningIntroCutscene or GameObject.tutorialIsRunning))
end
local function voiceOwner(resource)
  if not foregroundAllowed() then return end
  -- Do not infer coroutine ownership from a global stack top: engine callbacks
  -- may arrive while several yielding callers are active. Only one eligible
  -- resource-family scope may claim an exchange; ambiguity stays legacy.
  local found
  for _,ctx in ipairs(voiceScopes) do
    if ctx.epoch==activeEpoch and ctx.resources[resource:lower()] then
      if found then trace('voice_ambiguous_scope resource='..resource);return end
      found=ctx
    end
  end
  return found
end
local function identity(dlg, node, instance, count)
  local text = tostring(node)
  local id = text:match('^dlg_id:%s*(%d+)$') or text:match('^(%d+)$')
  if not id then return nil end
  local key = (tostring(instance) .. '.' .. id .. '.' .. tostring(count or 0)):gsub('%s+', '_')
  return tostring(dlg), id, key
end
local function beginExchange(dlg, node, instance, count)
  if type(COOP_DiagRawExchange) == 'function' then
    local _, _, key = identity(dlg, node, instance, count)
    COOP_DiagRawExchange('begin', dlg, node, instance, count, ready, key, key and sent[key] or false)
  end
  if guest then return end
  updateReady()
  if not ready then trace('skipped_begin_not_ready'); return end
  local resource, id, key = identity(dlg, node, instance, count)
  if not key then trace('unsupported_node_id=' .. tostring(node)); COOP_ExchangeAbort(); return end
  if sent[key] then return end
  if completed[key] then return end
  local numericCount = tonumber(count)
  if not numericCount or numericCount < 0 or numericCount % 1 ~= 0 then
    trace('invalid_execution_count'); COOP_ExchangeAbort(); return
  end
  local base = resource .. ':' .. tostring(instance) .. ':' .. id
  if executions[base] then
    trace('ambiguous_execution'); COOP_ExchangeAbort(); return
  end
  -- Query with the ORIGINAL engine node userdata, before any wire conversion.
  local ok, chore = pcall(DlgExchangeGetChore, dlg, node)
  if not ok or type(chore) ~= 'string' or chore == '' then
    trace('unresolved dlg=' .. tostring(dlg) .. ' node=' .. tostring(node))
    COOP_ExchangeAbort()
    return
  end
  local owner=voiceOwner(resource)
  if owner and COOP_VoiceOpen(key)~=1 then voiceFail('open_failed') end
  if COOP_ExchangeEmit(owner and 'voice_begin' or 'begin', resource, id, chore, key) == 1 then
    sent[key] = true
    executions[base] = {key=key, count=numericCount,instance=tostring(instance),voice=owner~=nil}
    if owner then owner.keys[#owner.keys+1]=key end
  end
end
local function endExchange(dlg, node, instance, count)
  if type(COOP_DiagRawExchange) == 'function' then
    local _, _, key = identity(dlg, node, instance, count)
    COOP_DiagRawExchange('end', dlg, node, instance, count, ready, key, key and sent[key] or false)
  end
  if guest then return end
  updateReady()
  if not ready then
    if COOP_DiagExchangeDecision then COOP_DiagExchangeDecision('end','not_ready',nil) end
    return
  end
  local resource, id, key = identity(dlg, node, instance, count)
  local base = key and (resource .. ':' .. tostring(instance) .. ':' .. id)
  local execution = base and executions[base]
  -- Native execution count advances at completion. Match the one active
  -- execution, never an arbitrary previous begin or a different dialog.
  if execution and tonumber(count) == execution.count + 1 and sent[execution.key] then
    key = execution.key
    if execution.voice and COOP_VoiceEnd(key)~=1 then voiceFail('end_failed') end
    local emitted = COOP_ExchangeEmit('end', resource, id, '-', key)
    if COOP_DiagExchangeDecision then
      COOP_DiagExchangeDecision('end',emitted == 1 and 'transmitted' or 'send_rejected',key)
    end
    sent[key] = nil
    executions[base] = nil
    completed[key] = true
  elseif COOP_DiagExchangeDecision then
    COOP_DiagExchangeDecision('end',key and 'unmatched_key' or 'invalid_identity',key)
  end
end

-- An added listener observes already-registered callbacks; replacing their
-- globals would not replace functions previously stored in the callback list.
Callback_DialogueExchangeNodeBegin:Add(beginExchange)
Callback_DialogueExchangeNodeEnd:Add(endExchange)
if voiceMode and not guest then
  local unpackValues=unpack or table.unpack
  local function pack(...) return {n=select('#',...),...} end
  local function wrapVoice(name,resourcesFor)
    local original=Dialogue[name]
    Dialogue[name]=function(self,...)
      if not foregroundAllowed() then return original(self,...) end
      local resources=resourcesFor(self,...)
      if not resources then return original(self,...) end
      local ready,epoch=updateReady();if not ready then return original(self,...) end
      if #voiceScopes>=8 then voiceFail('scope_overflow') end
      local ctx={epoch=epoch,resources=resources,keys={}}
      voiceScopes[#voiceScopes+1]=ctx
      -- No pcall: original engine calls may Yield under Lua5.1.
      local result=pack(original(self,...))
      for i,item in ipairs(voiceScopes) do if item==ctx then table.remove(voiceScopes,i);break end end
      for _,key in ipairs(ctx.keys) do
        local waiting=false
        while true do
          local active,current=updateReady()
          if not active or current~=ctx.epoch then voiceFail('session_changed') end
          local done=COOP_VoiceStatus(key)
          if done==1 then break end
          if done~=0 then voiceFail('completion_wait_failed') end
          if not waiting then trace('voice_wait key='..key);waiting=true end
          Yield()
        end
      end
      return unpackValues(result,1,result.n)
    end
  end
  wrapVoice('PlayPokerResponse',function() return {['responses.dlog']=true} end)
  wrapVoice('PlayPokerLine',function(_,initiator)
    local name=initiator and initiator.GetName and initiator:GetName():lower()
    if name=='brock' or name=='claptrap' or name=='ash' or name=='sam' then return {[name..'.dlog']=true} end
  end)
  wrapVoice('PlayLine',function(_,dlg,selector)
    if tostring(dlg):lower()=='dealer.dlog' and (selector=='dealer_hands' or selector=='dealer_handWinner') then return {['dealer.dlog']=true} end
  end)
  wrapVoice('PokerAction',function(_,initiator)
    if initiator=='Glados' then return {['dealer.dlog']=true} end
  end)
  local stop=DlgStop
  DlgStop=function(instance,...)
    for base,execution in pairs(executions) do
      if execution.voice and execution.instance==tostring(instance) and sent[execution.key] then
        if COOP_VoiceCancel(execution.key)~=1 then voiceFail('cancel_send_failed') end
        if COOP_ExchangeEmit('end',base:match('^(.-):'),base:match('([^:]+)$'),'-',execution.key)~=1 then voiceFail('cancel_end_send_failed') end
        sent[execution.key]=nil;executions[base]=nil;completed[execution.key]=true
      end
    end
    return stop(instance,...)
  end
end
if guest then
  -- The exchange stream owns test-mode dialogue. The base wrapper suppresses
  -- both string selectors AND userdata-node DlgRun calls during gameplay.
  Dialogue.StartBanter = function() end
  Dialogue.UpdateBanter = function() end
end

ThreadStart(function()
  while true do
    updateReady()
    if guest and ready then
      pollVoiceCancels()
      if finishing then
        local ok,playing=pcall(ControllerIsPlaying,controllers[finishing.key])
        if not ok then
          trace('finish_controller_error');COOP_ExchangeAbort();stopAll()
        elseif not playing then
          finishEnd(finishing);finishing=nil
        end
      end
      -- Bounded drain: a busy host/socket must never starve the game thread.
      -- Hold later replay/gates until the verified finite exit has completed.
      for i = 1, (finishing and 0 or 64) do
        local kind, eventEpoch, seq, dlg, node, authoredChore, key = COOP_ExchangeTake()
        if not kind then break end
        if eventEpoch ~= activeEpoch then
          trace('epoch_mismatch'); COOP_ExchangeAbort(); stopAll(); break
        end
        if (kind == 1 or kind == 11) and not controllers[key] then
          if kind==11 and not voiceMode then voiceFail('unexpected_protected_begin') end
          local skip=cancelledKeys[key]
          if kind==11 and not skip then
            local state=COOP_VoiceState(key)
            if state==2 then skip=true;cancelledKeys[key]=true
            elseif state~=1 then voiceFail('missing_protected_entry') end
          end
          -- String node IDs are not guaranteed accepted by every engine API.
          -- Prefer a verified local lookup; retain the exact host-resolved
          -- filename as fallback rather than choosing another graph variant.
          local ok, chore = pcall(DlgExchangeGetChore, dlg, 'dlg_id: ' .. node)
          if not ok or chore ~= authoredChore then
            chore = authoredChore
            trace('host_resource_fallback seq=' .. tostring(seq))
          end
          local played, controller=true,nil
          if not skip then played,controller=pcall(play,chore) end
          if not skip and (not played or not controller) then
            trace('play_failed seq=' .. tostring(seq) .. ' chore=' .. tostring(chore))
            COOP_ExchangeAbort(); stopAll(); break
          end
          controllers[key] = controller
          -- A delayed guest can still be in the authored departure when the
          -- host sends end. Keep verified finite exits and protected voice alive.
          naturalFinish[key] = kind==11 or hasAuthoredExit(dlg,node,authoredChore)
          voiceKeys[key]=kind==11
          if controller and type(COOP_DiagWatchController) == 'function' then COOP_DiagWatchController(key, controller) end
          trace((kind==11 and 'voice_begin' or 'begin')..' epoch=' .. tostring(activeEpoch) .. ' seq=' .. tostring(seq) .. ' key=' .. key .. ' chore=' .. chore)
        elseif kind == 2 then
          local controller = controllers[key]
          if controller and naturalFinish[key] and ControllerIsPlaying(controller) then
            finishing={epoch=activeEpoch,seq=seq,key=key}
            trace('finish_wait key='..key)
            break
          end
          if controller then stopController(controller) end
          finishEnd({epoch=activeEpoch,seq=seq,key=key})
        elseif kind == 3 or kind == 4 then
          if not COOP_PaceReceive then
            trace('pacing_not_installed'); COOP_ExchangeAbort(); stopAll(); break
          end
          COOP_PaceReceive(kind, key)
          trace((kind == 3 and 'gate_enter' or 'gate_release') .. ' epoch=' .. tostring(activeEpoch) .. ' seq=' .. tostring(seq) .. ' key=' .. key)
        elseif kind >= 5 and kind <= 7 then
          if not COOP_ShowdownReceive then
            trace('showdown_not_installed'); COOP_ExchangeAbort(); stopAll(); break
          end
          COOP_ShowdownReceive(kind,dlg,node,authoredChore,key)
          local names={[5]='show_node',[6]='show_done',[7]='reveal'}
          trace(names[kind]..' epoch='..tostring(activeEpoch)..' seq='..tostring(seq)..' key='..key)
        elseif kind >= 8 and kind <= 10 then
          if not COOP_BustoutReceive then
            trace('bustout_not_installed'); COOP_ExchangeAbort(); stopAll(); break
          end
          COOP_BustoutReceive(kind,dlg,node,authoredChore,key)
          local names={[8]='bust_chore',[9]='bust_state',[10]='bust_done'}
          trace(names[kind]..' epoch='..tostring(activeEpoch)..' seq='..tostring(seq)..' key='..key)
        end
      end
      if COOP_ShowdownTick then COOP_ShowdownTick() end
      if COOP_BustoutTick then COOP_BustoutTick() end
    end
    Yield()
  end
end)
COOP_ExchangeInstalled = true
trace('installed role=' .. (guest and 'guest' or 'host') .. ' version=6')
