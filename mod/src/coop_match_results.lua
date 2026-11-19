-- Wait for the authored winner presentation before bringing the group to
-- the front-end results. The lobby keeps the result across LoadScript.
if COOP_MatchResultsInstalled then return end
assert(Game and Game.OnPlayerWin and GameTable and GameTable.InitRound and GameTable.EndRound,
       'match result methods unavailable')
local _,phase,role=COOP_LobbyStatus()
assert(phase==3 and (role==0 or role==1),'match result lobby role unavailable')
local guest=role==1
local beginHand,endHand,playerWin=GameTable.InitRound,GameTable.EndRound,Game.OnPlayerWin
local ledger

local function recordHand(game)
  if not ledger or ledger.game~=game or game.handNumber==0 then
    ledger={game=game,busted={}}
  end
  ledger.starts={}
  for seat,player in ipairs(game.players) do ledger.starts[seat]=player:GetChips() end
end

local function recordBustes(game)
  assert(ledger and ledger.game==game,'missing match record')
  for seat,player in ipairs(game.players) do
    if player:IsBusted() and not ledger.busted[seat] then
      ledger.busted[seat]={seat=seat,hand=game.handNumber,chips=ledger.starts[seat]}
    end
  end
end

local function finalPlaces(game,winner)
  local ranks,eliminated={},{}
  for seat,player in ipairs(game.players) do
    if player==winner then ranks[seat]=1
    else eliminated[#eliminated+1]=assert(ledger.busted[seat],'missing elimination') end
  end
  table.sort(eliminated,function(a,b)
    if a.hand~=b.hand then return a.hand>b.hand end
    if a.chips~=b.chips then return a.chips>b.chips end
    return a.seat<b.seat
  end)
  local previous,rank
  for index,item in ipairs(eliminated) do
    if not previous or previous.hand~=item.hand or previous.chips~=item.chips then rank=index+1 end
    ranks[item.seat]=rank
    previous=item
  end
  return table.concat(ranks)
end

GameTable.InitRound=function(self,...)
  if not guest and COOP_SessionActive() then recordHand(self) end
  return beginHand(self,...)
end
GameTable.EndRound=function(self,...)
  if not guest and COOP_SessionActive() then recordBustes(self) end
  return endHand(self,...)
end
Game.OnPlayerWin=function(self,winner,...)
  if not COOP_SessionActive() then return playerWin(self,winner,...) end
  while DialogueObject:IsPokerLineRunning() do Yield() end
  local placement=guest and '' or finalPlaces(self:GetTable(),winner)
  if COOP_MatchAction('finish',placement)~=1 then
    COOP_ExchangeAbort()
    error('Multiplayer ended before results could be shared')
  end
  local _,state=COOP_LobbyStatus()
  while state==3 do
    Yield()
    _,state=COOP_LobbyStatus()
  end
  if state~=5 then error('Multiplayer ended while waiting for tournament results') end
  if not COOP_SessionEnd() or COOP_MatchAction('detach')~=1 then
    COOP_ExchangeAbort()
    error('Could not return to the multiplayer menu')
  end
  Logic.bInGame=false
  Logic.bAnotherNewGame=false
  MusicManager_KillMusic()
  Print('COOP_RESULTS loading_frontend')
  LoadScript('LaunchCelebrityPoker.lua')
end
COOP_MatchResultsInstalled=true
