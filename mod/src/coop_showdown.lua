-- Selected native chore-node replay. The guest never runs showdown.dlog.
-- Installed BEFORE pacing, so pacing surrounds these yielding local scopes.
if not (COOP_TestMode and COOP_TestMode()) or COOP_ShowdownInstalled then return end
assert(COOP_ExchangeInstalled and COOP_ShowdownNodes, 'showdown dependencies unavailable')
assert(Callback_DialogueNodeBegin and type(Callback_DialogueNodeBegin.Add)=='function', 'node callback unavailable')
assert(GameTable and type(GameTable.RunAllInBeforeRiverShowCardsChore)=='function', 'reveal method unavailable')
assert(Dialogue and type(Dialogue.PlayLine)=='function', 'dialogue unavailable')
assert(type(ChorePlayAndWait)=='function' and type(Drinks_PauseDrinks)=='function', 'showdown APIs unavailable')
local guest=COOP_IsGuest and COOP_IsGuest()
local completion=COOP_PresentationOptions and COOP_PresentationOptions()==1
if completion then
  assert(type(COOP_PresentationBegin)=='function' and type(COOP_PresentationFinish)=='function' and
         type(COOP_PresentationStatus)=='function','presentation completion APIs unavailable')
end
local unpackValues=unpack or table.unpack
local play, waitChore, pauseDrinks = ChorePlay, ChorePlayAndWait, Drinks_PauseDrinks
local line, reveal = Dialogue.PlayLine, GameTable.RunAllInBeforeRiverShowCardsChore
local nodes=COOP_ShowdownNodes
local graph, counters, queue, selections, seen, controllers = nil, {}, {}, {}, {}, {}
local selectionCount, controllerCount, generation = 0, 0, 0
local main={}
local revealContexts=setmetatable({}, {__mode='k'})
local function pack(...) return {n=select('#',...),...} end
local function trace(s) Print('COOP_SHOWDOWN '..s) end
local function fail(reason)
  trace('FAULT '..reason); COOP_ExchangeAbort()
  error('Co-op showdown stopped: '..reason)
end
local function playing(controller)
  local ok,value=pcall(ControllerIsPlaying,controller)
  if not ok then fail('controller_query_failed') end
  return value
end
local function ensure(ctx)
  local ready,epoch=COOP_ExchangeSync()
  if not ready or ctx.epoch~=epoch or ctx.generation~=generation then fail('session_changed') end
end
local function scope(kind,game)
  local ready,epoch=COOP_ExchangeSync()
  if not ready then fail('scope_before_ready') end
  local hand=game and game.handNumber
  if type(hand)~='number' then fail('missing_hand') end
  local lane=hand..'.'..tostring(Logic.iPokerState or 0)..'.'..kind
  counters[lane]=(counters[lane] or 0)+1
  return {key=lane..'.'..counters[lane],epoch=epoch,generation=generation,game=game}
end
local function completionBegin(ctx)
  if not completion then return end
  while true do
    ensure(ctx)
    local result=COOP_PresentationBegin(ctx.key)
    if result==1 then return end
    if result~=0 then fail('completion_scope_begin_failed') end
    Yield()
  end
end
local function completionFinish(ctx)
  if not completion then return end
  while true do
    ensure(ctx)
    local result=COOP_PresentationFinish(ctx.key)
    if result==1 then break end
    if result~=0 then fail('completion_scope_finish_failed') end
    Yield()
  end
  -- Guest ACK is sent BEFORE the outer pacing wrapper waits for host release.
  -- Host polls without holding the network lock; both socket readers keep up.
  if not guest then
    local waiting=false
    while true do
      ensure(ctx)
      local result=COOP_PresentationStatus(ctx.key)
      if result==1 then break end
      if result~=0 then fail('completion_scope_wait_failed') end
      if not waiting then trace('completion_wait key='..ctx.key);waiting=true end
      Yield()
    end
  end
  trace('completion_done key='..ctx.key..' role='..(guest and 'guest' or 'host'))
end
local function send(kind,node,resource,key,ctx)
  ensure(ctx)
  if COOP_ExchangeEmit(kind,kind=='reveal' and 'reveal' or 'showdown.dlog',node,resource,key)~=1 then
    fail('send_failed')
  end
end
COOP_ShowdownReset=function()
  for key,controller in pairs(controllers) do
    local ok,value=pcall(ControllerIsPlaying,controller)
    if ok and value then pcall(ControllerKill,controller) end
  end
  graph, counters, queue, selections, seen, controllers = nil, {}, {}, {}, {}, {}
  revealContexts=setmetatable({}, {__mode='k'})
  selectionCount=0; controllerCount=0; generation=generation+1
  trace('reset generation='..generation)
end
local chords={A=true,B=true,Bb=true,['C#']=true,C=true,D=true,['G#']=true,G=true}
local function revealSeat(game,resource)
  for seat=1,4 do
    local player=game and game.players and game.players[seat]
    local base=player and player.playerData and player.playerData[kPlayerDataIndex_ShowdownChore]
    if type(base)=='string' and resource:sub(1,#base+1)==base..'_' and chords[resource:sub(#base+2)] then
      return seat
    end
  end
end
COOP_ShowdownReceive=function(kind,dlg,node,resource,key)
  if not guest then return fail('host_received_presentation') end
  local unique=kind..':'..key
  if seen[unique] then return fail('duplicate_presentation') end
  if #queue+selectionCount>=256 then return fail('pending_overflow') end
  if kind==5 then
    local entry=nodes[node]
    local prefix,instance,identity,count=key:match('^(%d+%.%d+%.zoom%.%d+):(%d+)%.(%d+)%.(%d+)$')
    if dlg~='showdown.dlog' or not prefix or identity~=node or not entry or entry.op=='control' or entry.resource~=resource then
      return fail('invalid_node_presentation')
    end
    queue[#queue+1]={kind=kind,scope=prefix,key=key,node=node,entry=entry}
  elseif kind==6 then
    if dlg~='showdown.dlog' or node~='0' or resource~='-' or not key:match('^%d+%.%d+%.zoom%.%d+$') then
      return fail('invalid_scope_end')
    end
    queue[#queue+1]={kind=kind,scope=key,key=key}
  elseif kind==7 then
    local prefix,seat=key:match('^(%d+%.%d+%.reveal%.%d+):([1-4])$')
    local game=GameObject and GameObject:GetTable()
    if dlg~='reveal' or not prefix or node~=seat or revealSeat(game,resource)~=tonumber(seat) then
      return fail('invalid_reveal_selection')
    end
    selections[key]=resource; selectionCount=selectionCount+1
  else return fail('invalid_presentation_kind') end
  seen[unique]=true
end
local function start(entry,key)
  if entry.op=='pause' then
    pauseDrinks()
    trace('pause key='..key)
    return
  end
  -- Chore-node priority is authored as 100; music scripts use API defaults.
  if controllerCount>=256 then fail('controller_overflow') end
  local ok,controller
  if entry.op=='chore' then ok,controller=pcall(play,entry.resource,100)
  else ok,controller=pcall(play,entry.resource) end
  if not ok or not controller then fail('play_failed resource='..entry.resource) end
  controllers[key]=controller
  controllerCount=controllerCount+1
  if entry.op=='chore' then graph.controller=controller end
  if COOP_DiagWatchController then COOP_DiagWatchController('show:'..key,controller) end
  trace('play op='..entry.op..' key='..key..' chore='..entry.resource)
end
COOP_ShowdownTick=function()
  if not guest then return end
  -- Reap natural completions, including music that outlives its graph scope.
  for key,controller in pairs(controllers) do
    if not playing(controller) then controllers[key]=nil; controllerCount=controllerCount-1 end
  end
  if not graph then return end
  ensure(graph)
  if graph.controller and not playing(graph.controller) then graph.controller=nil end
  for i=1,16 do
    local item=queue[1]
    if not item or item.scope~=graph.key then break end
    if graph.done then return fail('node_after_scope_end') end
    -- When packets accumulate, preserve sequential camera choreography rather
    -- than starting every queued zoom on the same frame. Music is independent.
    if item.kind==5 and item.entry.op=='chore' and graph.controller then break end
    table.remove(queue,1)
    if item.kind==6 then
      graph.done=true
      trace('graph_end key='..item.key)
    else start(item.entry,item.key) end
  end
end
Callback_DialogueNodeBegin:Add(function(dlg,node,instance,count)
  if guest or not graph or not (Logic and Logic.bInGame==true) or tostring(dlg)~='showdown.dlog' then return end
  ensure(graph)
  local id=tostring(node):match('^dlg_id:%s*(%d+)$') or tostring(node):match('^(%d+)$')
  local entry=id and nodes[id]
  if not entry then return fail('unknown_showdown_node='..tostring(node)) end
  if entry.op=='control' then return end
  if type(instance)~='number' or instance<0 or instance%1~=0 or type(count)~='number' or count<0 or count%1~=0 then
    return fail('invalid_node_execution')
  end
  local key=graph.key..':'..tostring(instance)..'.'..id..'.'..tostring(count)
  if seen[key] then return end
  send('show_node',id,entry.resource,key,graph)
  seen[key]=true
  trace('selected op='..entry.op..' key='..key..' chore='..entry.resource)
end)
Dialogue.PlayLine=function(self,dlg,selector,...)
  if not (Logic and Logic.bInGame==true) or dlg~='showdown.dlog' or selector~='showdownZoomIn' then
    return line(self,dlg,selector,...)
  end
  if graph then return fail('overlapping_graph_calls') end
  local ctx=scope('zoom',GameObject:GetTable())
  graph=ctx
  trace('graph_enter key='..ctx.key)
  completionBegin(ctx)
  local result=pack(line(self,dlg,selector,...))
  if guest then
    -- The baseline guest graph returns immediately. Keep its game coroutine
    -- inside this call until host return AND the selected local chore finishes.
    while not ctx.done or ctx.controller do ensure(ctx); Yield() end
  else send('show_done','0','-',ctx.key,ctx) end
  completionFinish(ctx)
  ensure(ctx)
  graph=nil
  trace('graph_complete key='..ctx.key)
  return unpackValues(result,1,result.n)
end
GameTable.RunAllInBeforeRiverShowCardsChore=function(self,...)
  if not (Logic and Logic.bInGame==true) then return reveal(self,...) end
  local thread=coroutine.running() or main
  if revealContexts[thread] then return fail('nested_reveal') end
  local ctx=scope('reveal',self)
  revealContexts[thread]=ctx
  completionBegin(ctx)
  local result=pack(reveal(self,...))
  completionFinish(ctx)
  ensure(ctx)
  revealContexts[thread]=nil
  return unpackValues(result,1,result.n)
end
ChorePlayAndWait=function(resource,...)
  local ctx=revealContexts[coroutine.running() or main]
  if not ctx or type(resource)~='string' then return waitChore(resource,...) end
  local seat=revealSeat(ctx.game,resource)
  if not seat then return waitChore(resource,...) end
  ensure(ctx)
  local key=ctx.key..':'..seat
  if ctx[seat] then return fail('duplicate_reveal_seat') end
  ctx[seat]=true
  if guest then
    trace('reveal_wait key='..key)
    while not selections[key] do ensure(ctx); Yield() end
    resource=selections[key]; selections[key]=nil; selectionCount=selectionCount-1
  else send('reveal',tostring(seat),resource,key,ctx) end
  -- Keep the original local reveal/camera/UI flow. Its independent chord draw
  -- is discarded on guest; the actual played resource is always host-selected.
  trace('reveal_play key='..key..' chore='..resource)
  return waitChore(resource,...)
end
COOP_ShowdownInstalled=true
trace('installed version=3 completion='..(completion and '1' or '0'))
