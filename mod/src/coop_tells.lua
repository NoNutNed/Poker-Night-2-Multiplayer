-- Keep the shipped random tell selection and send its public animation choices.
if COOP_TellsInstalled or not COOP_HandReadyInstalled then return end
assert(COOP_ControlInstalled and COOP_ExchangeSync and type(GameTable.ShowTells)=='function',
  'tell synchronization dependencies unavailable')

local guest=COOP_IsGuest and COOP_IsGuest()
local show,decide=GameTable.ShowTells,AIPlayer.EvaluateAndAct
local streets={ModePreFlop=1,ModeFlop=2,ModeTurn=3,ModeRiver=4}
local counters,pending,seen,controllers={},{},{},{}
local pendingCount=0

local function fault(reason)
  Print('COOP_TELLS FAULT '..reason)
  COOP_ExchangeAbort()
end

local function sameEpoch(epoch)
  local ready,current=COOP_ExchangeSync()
  return ready and current==epoch
end

local function eligible(player)
  return player and not COOP_HumanOwns(player) and not player:IsFolded() and not player:IsBusted()
end

local function allowed(player,resource)
  return resource==player.tellsGood[1] or resource==player.tellsBad[1] or
    resource==player.tellsNeutral[1] or resource==player.tellsNeutral[2]
end

COOP_TellReset=function()
  for player,controller in pairs(controllers) do
    local ok,playing=pcall(ControllerIsPlaying,controller)
    if ok and playing then pcall(ControllerKill,controller) end
    if player.tellChoreController==controller then player.tellChoreController=nil end
  end
  counters,pending,seen,controllers={},{},{},{}
  pendingCount=0
end

COOP_TellReceive=function(dlg,node,value,key)
  local a,b,c,d=value:match('^([^|]+)|([^|]+)|([^|]+)|([^|]+)$')
  if not guest or dlg~='tell' or node~='0' or not d or
     not key:match('^%d+%.[1-4]%.tell%.%d+$') or seen[key] or pendingCount>=16 then
    return fault('invalid_batch')
  end
  seen[key]=true
  pending[key]={a,b,c,d}
  pendingCount=pendingCount+1
end

local function selectTells(game,state,key,epoch)
  local selected={'-','-','-','-'}
  local owners={}
  for seat=1,4 do
    local player=game.players[seat]
    if eligible(player) then
      for _,resource in ipairs({player.tellsGood[1],player.tellsBad[1],player.tellsNeutral[1],player.tellsNeutral[2]}) do
        owners[resource]=seat
      end
    end
  end
  -- The shipped ShowTells loop and look-at reset do not yield. Scope
  -- interception to that call so dialogue and background chores stay separate.
  local play=ChorePlay
  ChorePlay=function(resource,...)
    local controller=play(resource,...)
    local seat=owners[resource]
    if seat and controller then
      selected[seat]=resource
      controllers[game.players[seat]]=controller
    end
    return controller
  end
  local ok,err=pcall(show,game,state)
  ChorePlay=play
  if not ok then return fault('selection_failed '..tostring(err)) end
  if not sameEpoch(epoch) then return end
  local batch=table.concat(selected,'|')
  if COOP_ExchangeEmit('tell','tell','0',batch,key)~=1 then return fault('send_failed') end
  Print('COOP_TELLS selected key='..key..' chores='..batch)
end

local function replayTells(game,key,epoch)
  while not pending[key] do
    if not sameEpoch(epoch) then return end
    Yield()
  end
  if not sameEpoch(epoch) then return end
  local selected=pending[key]
  pending[key]=nil
  pendingCount=pendingCount-1
  for seat,resource in ipairs(selected) do
    local player=game.players[seat]
    if resource~='-' and (not eligible(player) or player.tellChoreController or not allowed(player,resource)) then
      return fault('invalid_seat_or_resource key='..key..' seat='..seat)
    end
  end
  for seat,resource in ipairs(selected) do
    if resource~='-' then
      local player=game.players[seat]
      LookatBoxManager_ResetPlayerToZero(kPlayerIndexMap[player:GetName()])
      local ok,controller=pcall(ChorePlay,resource)
      if not ok or not controller then return fault('play_failed key='..key..' seat='..seat) end
      player.tellChoreController=controller
      controllers[player]=controller
    end
  end
  Print('COOP_TELLS replayed key='..key..' chores='..table.concat(selected,'|'))
end

GameTable.ShowTells=function(self,state)
  if not (Logic and Logic.bInGame==true and COOP_SessionActive()) then return show(self,state) end
  local ready,epoch=COOP_ExchangeSync()
  if not ready then return fault('street_before_ready') end
  local street=streets[state.name]
  if not street then return fault('invalid_street') end
  local lane=self.handNumber..'.'..street..'.tell'
  counters[lane]=(counters[lane] or 0)+1
  local key=lane..'.'..counters[lane]
  if guest then return replayTells(self,key,epoch) end
  return selectTells(self,state,key,epoch)
end

if guest then
  AIPlayer.EvaluateAndAct=function(self,...)
    local controller=controllers[self]
    if controller and self.tellChoreController==controller and ControllerIsPlaying(controller) then
      local ready,epoch=COOP_ExchangeSync()
      if not ready then return end
      repeat
        Yield()
        if not sameEpoch(epoch) then return end
      until not ControllerIsPlaying(controller)
    end
    return decide(self,...)
  end
end

COOP_TellsInstalled=true
