-- Generic dealer elimination side effects, separate from exchange voice replay.
-- Install before pacing so the host gate surrounds this yielding local scope.
if not (COOP_TestMode and COOP_TestMode()) or COOP_BustoutInstalled then return end
assert(COOP_ExchangeInstalled and COOP_BustoutNodes, 'bust-out dependencies unavailable')
assert(Callback_DialogueNodeBegin and Callback_DialogueNodeEnd, 'node callbacks unavailable')
assert(Dialogue and type(Dialogue.PlayLine)=='function', 'dialogue unavailable')
assert(type(AgentFind)=='function' and type(AgentGetProperty)=='function' and
       type(AgentSetProperty)=='function' and type(AgentHide)=='function', 'actor state APIs unavailable')
local guest=COOP_IsGuest and COOP_IsGuest()
local syncSharedIdle=COOP_HandReadyInstalled==true
local unpackValues=unpack or table.unpack
local line,play=Dialogue.PlayLine,ChorePlay
local nodes=COOP_BustoutNodes
local active,counters,queue,seen,controllers=nil,{},{},{},{}
local generation,controllerCount=0,0
local function pack(...) return {n=select('#',...),...} end
local function trace(s) Print('COOP_BUSTOUT '..s) end
local function fail(reason)
  trace('FAULT '..reason); COOP_ExchangeAbort()
  error('Co-op bust-out stopped: '..reason)
  return false
end
local function ensure(ctx)
  local ready,epoch=COOP_ExchangeSync()
  if not ready or epoch~=ctx.epoch or generation~=ctx.generation then
    if active==ctx then active=nil end
    return fail('session_changed')
  end
  return true
end
local function seatFor(game,name)
  for seat=1,4 do
    local player=game and game.players and game.players[seat]
    if player and player:GetName()==name then return seat end
  end
end
COOP_BustoutReset=function(keepControllers)
  if not keepControllers then
    for key,controller in pairs(controllers) do
      local ok,value=pcall(ControllerIsPlaying,controller)
      if ok and value then pcall(ControllerKill,controller) end
    end
    controllers={}; controllerCount=0
  end
  active,counters,queue,seen=nil,{},{},{}
  generation=generation+1
  trace('reset keep_departures='..tostring(keepControllers==true))
end
local function send(kind,node,value,index)
  local ctx=active
  if not ensure(ctx) then return false end
  ctx.serial=ctx.serial+1
  local key=ctx.key..':'..ctx.serial..':'..index
  if COOP_ExchangeEmit(kind,'dealer.dlog',node,value,key)~=1 then return fail('send_failed') end
  trace('selected op='..kind..' key='..key..' value='..value)
  return true
end
local function execution(phase,dlg,node,instance,count)
  if guest or not active or not (Logic and Logic.bInGame==true) or tostring(dlg)~='dealer.dlog' then return end
  local id=tostring(node):match('^dlg_id:%s*(%d+)$') or tostring(node):match('^(%d+)$')
  local entry=id and nodes[id]
  if not entry then return end
  local shared=entry.op=='shared_idle'
  if shared and not syncSharedIdle then return end
  if not ensure(active) then return end
  if not shared and entry.seat~=active.seat then return fail('wrong_departure_seat node='..id..' seat='..active.seat) end
  if type(instance)~='number' or instance<0 or instance%1~=0 or type(count)~='number' or count<0 or count%1~=0 then
    return fail('invalid_node_execution')
  end
  local unique=phase..':'..id..':'..instance..':'..count
  if active.seen[unique] then return end
  active.seen[unique]=true
  if phase=='begin' and entry.op=='chore' then
    if not send('bust_chore',id,entry.resource,0) then return end
    active.departure=true
  elseif phase=='end' and entry.props then
    for index,prop in ipairs(entry.props) do
      local ok,actor=pcall(AgentFind,prop.actor)
      if not ok then return fail('actor_lookup_failed='..prop.actor) end
      if not actor then
        if prop.actor:match('^obj_') then trace('absent_optional_prop='..prop.actor)
        else return fail('missing_actor='..prop.actor) end
      else
        local value
        if prop.authored and syncSharedIdle then
          -- Claptrap's departure chore is still driving his background actor
          -- when #353 ends. The transient host sample was false even though
          -- #353 unconditionally makes the background actor visible.
          value=prop.value and '1' or '0'
        elseif prop.key=='visible' then
          local read,visible=pcall(AgentGetProperty,prop.actor,'Runtime: Visible')
          if not read or type(visible)~='boolean' then return fail('visibility_read_failed='..prop.actor) end
          value=visible and '1' or '0'
        else
          -- Idle writes in these verified nodes are unconditional authored
          -- AnimOrChore handles. Send their known filename, never a Lua pointer.
          value=prop.value=='' and '-' or prop.value
        end
        if not send('bust_state',id,value,index) then return end
      end
    end
    if entry.final then active.final=true end
  end
end
Callback_DialogueNodeBegin:Add(function(...) execution('begin',...) end)
Callback_DialogueNodeEnd:Add(function(...) execution('end',...) end)
COOP_BustoutReceive=function(kind,dlg,node,value,key)
  if not guest then return fail('host_received_bustout') end
  if seen[key] then return fail('duplicate_operation') end
  if #queue>=256 then return fail('pending_overflow') end
  local scope,seat,serial,index=key:match('^(%d+%.%d+%.bust%.([1-4])%.%d+):(%d+):(%d+)$')
  serial,index=tonumber(serial),tonumber(index)
  if dlg~='dealer.dlog' or not scope or not serial or serial<1 or not index then return fail('invalid_identity') end
  local entry=nodes[node]
  if kind==8 then
    if not entry or entry.op~='chore' or entry.seat~=tonumber(seat) or index~=0 or value~=entry.resource then
      return fail('invalid_departure')
    end
  elseif kind==9 then
    local prop=entry and entry.props and entry.props[index]
    local allowed=entry and ((entry.op=='state' and entry.seat==tonumber(seat)) or
                              (entry.op=='shared_idle' and syncSharedIdle))
    if not allowed or not prop or
       (prop.key=='visible' and value~='0' and value~='1') or
       (prop.authored and syncSharedIdle and value~=(prop.value and '1' or '0')) or
       (prop.key=='idle' and value~=(prop.value=='' and '-' or prop.value)) then
      return fail('invalid_actor_state')
    end
  elseif kind==10 then
    if node~='0' or value~='-' or index~=0 then return fail('invalid_completion') end
  else return fail('invalid_kind') end
  queue[#queue+1]={kind=kind,scope=scope,serial=serial,index=index,entry=entry,value=value,key=key}
  seen[key]=true
end
COOP_BustoutTick=function()
  if not guest then return end
  for key,controller in pairs(controllers) do
    local ok,value=pcall(ControllerIsPlaying,controller)
    if not ok then return fail('controller_query_failed') end
    if not value then controllers[key]=nil; controllerCount=controllerCount-1 end
  end
  if not active then return end
  if not ensure(active) then return end
  for i=1,16 do
    local item=queue[1]
    if not item or item.scope~=active.key then break end
    if active.done or item.serial~=active.serial+1 then return fail('operation_order') end
    table.remove(queue,1)
    active.serial=item.serial
    if item.kind==8 then
      if controllerCount>=32 then return fail('controller_overflow') end
      local ok,controller=pcall(play,item.value)
      if not ok or not controller then return fail('departure_play_failed') end
      controllers[item.key]=controller; controllerCount=controllerCount+1
      active.departure=true
      trace('play key='..item.key..' chore='..item.value)
    elseif item.kind==9 then
      local prop=item.entry.props[item.index]
      local ok,actor=pcall(AgentFind,prop.actor)
      if not ok or not actor then return fail('missing_guest_actor='..prop.actor) end
      if prop.key=='visible' then
        ok=pcall(AgentHide,prop.actor,item.value=='0')
      else
        ok=pcall(AgentSetProperty,prop.actor,'Walk Animation - Idle',item.value=='-' and '' or item.value)
      end
      if not ok then return fail('actor_write_failed='..prop.actor) end
      if item.entry.final then active.final=true end
      trace('state key='..item.key..' actor='..prop.actor..' property='..prop.key..' value='..item.value)
    else
      if not active.departure or not active.final then return fail('incomplete_actor_handoff') end
      active.done=true
      trace('handoff_done key='..item.key)
    end
  end
end
Dialogue.PlayLine=function(self,dlg,selector,...)
  if not (Logic and Logic.bInGame==true) or dlg~='dealer.dlog' or selector~='dealer_playerEliminated' then
    return line(self,dlg,selector,...)
  end
  local game=GameObject:GetTable()
  local seat=seatFor(game,Logic.sCurrentBustedPlayer)
  if not seat then return line(self,dlg,selector,...) end -- human elimination has a different owner
  if active then return fail('overlapping_eliminations') end
  local expansion=AgentGetProperty('logic_persistent','Current Expansion Dialog File')
  if expansion~='expansion_generic.dlog' and expansion~='dealer.dlog' then
    return fail('unsupported_expansion='..tostring(expansion))
  end
  local ready,epoch=COOP_ExchangeSync()
  if not ready then return fail('scope_before_ready') end
  local lane=game.handNumber..'.'..tostring(Logic.iPokerState or 0)..'.bust.'..seat
  counters[lane]=(counters[lane] or 0)+1
  local ctx={key=lane..'.'..counters[lane],seat=seat,serial=0,seen={},epoch=epoch,generation=generation}
  active=ctx
  trace('enter key='..ctx.key)
  local result=pack(line(self,dlg,selector,...))
  if guest then
    while not ctx.done do
      if not ensure(ctx) then return end
      Yield()
    end
  else
    if not ensure(ctx) then return end
    if not ctx.departure or not ctx.final then return fail('unsupported_missing_handoff') end
    if not send('bust_done','0','-',0) then return end
  end
  if not ensure(ctx) then return end
  active=nil
  -- Departure is nonblocking and may continue after this call or a hand reset.
  -- Background idles belong to the engine's actor properties, not this scope.
  trace('complete key='..ctx.key)
  return unpackValues(result,1,result.n)
end
COOP_BustoutInstalled=true
trace('installed version=7')
