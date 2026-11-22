-- Guest-local card presentation. Actual hands, identities, betting, native
-- selected presentation and the trusted full-deck transport remain unchanged.
if not (COOP_TestMode and COOP_TestMode()) or COOP_CardUIInstalled or
   not COOP_ControlOptions or COOP_ControlOptions()~=1 then return end
assert(COOP_ControlInstalled and COOP_ControlLocalPlayer,'controls must install first')
if not (COOP_IsGuest and COOP_IsGuest()) and
   not (COOP_LobbyStatus and select(15,COOP_LobbyStatus())==1) then COOP_CardUIInstalled=true;return end
assert(Player and type(Player.AddHoleCard)=='function' and CardRenderer_Render,'card renderer not loaded')
assert(AIPlayer and AIPlayer.Fold and AIPlayer.ShowHandUI and AIPlayer.HideHandUI and
       HumanPlayer and type(HumanPlayer.EvaluateAndAct)=='function' and HumanPlayer.Fold and HumanPlayer.RunCardCallout and HumanPlayer.RunCardCalloutClose,
       'card UI methods not loaded')
local render,add=CardRenderer_Render,Player.AddHoleCard
local play,waitChore=ChorePlay,ChorePlayAndWait
local callout,closeCallout=HumanPlayer.RunCardCallout,HumanPlayer.RunCardCalloutClose
local scopes=setmetatable({}, {__mode='k'})
local main={}
local localHandOwner
local unpackValues=unpack or table.unpack
local function pack(...) return {n=select('#',...),...} end
local function thread() return coroutine.running() or main end
local function game()
  if Logic and Logic.bInGame==true and GameObject then return GameObject:GetTable() end
end
local function owner()
  local g=game();return g and COOP_ControlLocalPlayer(g)
end
local function hostPlayer(self)
  local g=game();return g and self==g.players[5]
end
local function localSlot(agent)
  local name=type(agent)=='string' and agent or AgentGetName(agent)
  return name and tonumber(name:match('^obj_cardPokerPlayer([1-4])$'))
end
CardRenderer_Render=function(agent,card)
  if game() then
    local slot=localSlot(agent)
    if slot then
      local p=owner();local actual=p and p:GetHoleCards():Get(slot)
      if not actual then AgentHide(agent,true);return end
      -- This feeds the original UI reverse map too, so owner best-hand
      -- callouts resolve to these local meshes, not the host's old hand.
      return render(agent,actual)
    end
  end
  return render(agent,card)
end
local function suppress(resource)
  local scope=scopes[thread()]
  if not scope then return false end
  if scope=='host_action' then return resource=='ui_status_Player_activate.chore' end
  if scope=='fold' then return resource=='ui_player_fold.chore' or resource=='ui_player_fold_omaha.chore' end
  if scope=='deal' then
    local g=game();local prefix=g and g.players[5].playerData[kPlayerDataIndex_DealCardChore]
    if prefix then for i=1,4 do if resource==prefix..i or resource==prefix..i..'.chore' then return true end end end
  end
  return false
end
ChorePlay=function(resource,...) if suppress(resource) then return end;return play(resource,...) end
ChorePlayAndWait=function(resource,...) if suppress(resource) then return end;return waitChore(resource,...) end
-- The legacy host-action replica calls Player.EvaluateAndAct, whose normal
-- activation chore opens the local input panel. Keep the simulation/public
-- badge effects, but that panel belongs to this guest's own cast turn.
local hostEvaluate=HumanPlayer.EvaluateAndAct
HumanPlayer.EvaluateAndAct=function(self,...)
  local key=thread();local previous=scopes[key]
  if hostPlayer(self) then scopes[key]='host_action' end
  local result=pack(hostEvaluate(self,...))
  scopes[key]=previous
  return unpackValues(result,1,result.n)
end
Player.AddHoleCard=function(self,card,index,...)
  local key=thread();local previous=scopes[key]
  if hostPlayer(self) then scopes[key]='deal' end
  -- Keep the original AddHoleCard, cast deal, sleep and engine hand mutation.
  local result=pack(add(self,card,index,...))
  scopes[key]=previous
  if self==owner() and self:GetHoleCards():Get(index)==card and not Logic.bModeInstant then
    render('obj_cardPokerPlayer'..index,card)
    AgentHide('obj_cardPokerPlayer'..index,false)
    -- Animate each local hole card at its OWN seat's deal, not the host's.
    local g=game();play(g.players[5].playerData[kPlayerDataIndex_DealCardChore]..index)
  end
  return unpackValues(result,1,result.n)
end
local hostFold=HumanPlayer.Fold
HumanPlayer.Fold=function(self,...)
  local key=thread();local previous=scopes[key]
  if hostPlayer(self) then scopes[key]='fold' end
  local result=pack(hostFold(self,...))
  scopes[key]=previous
  local p=owner()
  if hostPlayer(self) and p then AgentSetSelectable('ui_player_buttonFoldCardPeek',p:IsFolded()) end
  return unpackValues(result,1,result.n)
end
local castFold=AIPlayer.Fold
AIPlayer.Fold=function(self,...)
  local result=pack(castFold(self,...))
  if self==owner() then
    play('ui_player_fold.chore')
    AgentSetSelectable('ui_player_buttonFoldCardPeek',true)
  end
  return unpackValues(result,1,result.n)
end
-- The Player's public hand/status UI still runs with the REAL host hand.
-- Its local-only best-five callout now belongs to the guest's owned cast seat.
HumanPlayer.RunCardCallout=function(self,...) if game() then return end;return callout(self,...) end
HumanPlayer.RunCardCalloutClose=function(self,...) if game() then return end;return closeCallout(self,...) end
local showHand,hideHand=AIPlayer.ShowHandUI,AIPlayer.HideHandUI
AIPlayer.ShowHandUI=function(self,...)
  local result=pack(showHand(self,...))
  if self==owner() then callout({cHand=self.cHand});localHandOwner=self end
  return unpackValues(result,1,result.n)
end
AIPlayer.HideHandUI=function(self,...)
  local result=pack(hideHand(self,...))
  if self==localHandOwner then
    if game() then closeCallout({cHand=self.cHand}) end
    localHandOwner=nil
  end
  return unpackValues(result,1,result.n)
end
COOP_CardUIInstalled=true
Print('COOP_CARD_UI installed owner_panel=1 trusted_full_deck=1')
