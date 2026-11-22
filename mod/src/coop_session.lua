-- Temporary clean multiplayer profile. Install before the menu, activate only
-- after lobby commit, and restore before a scene-script reload leaves the match.
if COOP_SessionInstalled then return end
for _,name in ipairs({'GetPreferences','SavePrefs','SaveGame','LoadScript','EngineQuit','RegisterShutdownCallback','PropertyGetKeys','PropertyIsLocal','PropertyRemove','SymbolCompare','COOP_ProtectPreferences','AgentFind','AgentExists','AgentHide','AgentSetProperty','ResetGameStateProperties','RulesExecute','CelebrityPoker_RunEndTournament','Themes_GetCurrentTheme'}) do
  assert(type(_G[name])=='function','session dependency unavailable: '..name)
end
assert(GameTable and type(GameTable.InitializeAgents)=='function' and type(GameTable.SaveGame)=='function','session table unavailable')
assert(type(TableBets_AreActive)=='function','session table bets unavailable')
assert(Game and GameObject,'session game unavailable')
assert(Dialogue and type(Dialogue.PlayIntroCutscene)=='function','session intro unavailable')
assert(AIPlayer and type(AIPlayer.CheckHoleCards)=='function','card-check animation unavailable')
for _,name in ipairs({'TableBets_Activate','TableBets_Open','TableBets_ClearAll','TableBets_KeyTriggered',
 'PlatformUnlockAchievement','Drinks_StartTutorial','HumanPlayerUI_SlideMainToBounty','HumanPlayerUI_OnButtonTriggerRDown',
 'InGameMenu_BountyUnlock','InGameMenu_BountyAwardNotify'}) do
  assert(type(_G[name])=='function','session dependency unavailable: '..name)
end
local active,castOnly=false,false
local savedPrefs,savedPersistent,prefs,persistent,watchedTable
local pendingSave=false
local protectPreferences=COOP_ProtectPreferences
local function trace(message) if type(Print)=='function' then Print('COOP_SESSION '..message) end end
local settingKeys={
  'Volume Master','Volume Music','Volume Sound','Volume Voice','Volume Ambient',
  'Settings - Enable After Effects','Settings - Graphics Quality','Settings - Shadow Quality',
  'Enable Subtitles','Windowed','Texture Quality','FX anti-aliasing','Game Window Position',
  'Usage OS','Usage CPU','Usage GPU','Usage Launch Count','Usage Shutdown Count',
  'Usage Session Execution Time','Usage Total Execution Time'
}
local function userSetting(key)
  if type(key)=='string' and (key:match('^Settings %- ') or key:match('^Volume ') or key:match('^Usage ')) then return true end
  for _,name in ipairs(settingKeys) do if SymbolCompare(key,name) then return true end end
end
local function snapshot(props,keepSetting)
  local result={}
  for _,key in ipairs(PropertyGetKeys(props)) do
    if PropertyIsLocal(props,key) and not (keepSetting and keepSetting(key)) then
      local value=props[key]
      local kind=type(value)
      result[#result+1]={key=key,value=value,scalar=kind=='boolean' or kind=='number' or kind=='string'}
    end
  end
  return result
end
local function hasKey(values,key)
  -- PN2 returns fresh native Symbol wrappers, so Lua identity is insufficient.
  for _,row in ipairs(values) do if SymbolCompare(row.key,key) then return true end end
  return false
end
local function sameValue(a,b)
  return a==b or (type(a)=='number' and type(b)=='number' and a~=a and b~=b)
end
local function restore(props,values,keepSetting)
  -- GetKeys includes inherited keys. Removing newly created local overrides
  -- reveals the inherited value and preserves the original serialized layout.
  for _,key in ipairs(PropertyGetKeys(props)) do
    if PropertyIsLocal(props,key) and not (keepSetting and keepSetting(key)) and not hasKey(values,key) then
      PropertyRemove(props,key)
      assert(not PropertyIsLocal(props,key),'could not remove new override: '..tostring(key))
    end
  end
  for _,row in ipairs(values) do
    if row.scalar and (not PropertyIsLocal(props,row.key) or not sameValue(props[row.key],row.value)) then
      props[row.key]=row.value
      assert(PropertyIsLocal(props,row.key) and sameValue(props[row.key],row.value),
        'could not restore local property: '..tostring(row.key))
    end
  end
end
local function neutral()
  Logic.iCurrentSpecialItem=0
  Logic.iPlayerWithSpecialItemAvailable=0
  Logic['Currently Earning Bounty Item']=false
  persistent['Table Bets - Set Active']=false
  persistent['Table Bets - Set Completed']=false
  for i=1,3 do
    persistent['Table Bets - Active Bet '..i]=0
    persistent['Table Bets - Active Bet '..i..' Complete']=false
  end
end
local function hideChallenges()
  for i=1,3 do
    for _,prefix in ipairs({'ui_bounty_background','ui_player_slider_bounty_text','ui_player_slider_bountyPopup_text','ui_menu_unlocks_gridCheck','ui_menu_unlocks_gridPopupCheck'}) do
      local name=prefix..i;if AgentExists(name) then AgentHide(name,true) end
    end
  end
  for _,name in ipairs({'ui_xbox_button_RT','ui_xbox_button_RT_alt'}) do if AgentExists(name) then AgentHide(name,true) end end
end
function COOP_SessionActive() return active end
function COOP_CastOnly() return active and castOnly end
function COOP_SessionBegin(cast)
  if active then return end
  prefs=GetPreferences();persistent=AgentFind('logic_persistent').mProps
  savedPrefs=snapshot(prefs,userSetting);savedPersistent=snapshot(persistent)
  protectPreferences('1')
  active=true;castOnly=cast==true;pendingSave=false
  prefs['Episode - Save Is Valid']=false
  prefs['Showed Popup - Tutorial']=true
  prefs['Episode - Chance of Unlock']=0
  prefs['Episode - Difficulty']=kDifficulty_Easy
  prefs.iCurrentActiveTheme=1
  prefs['Active Felt Index']=1;prefs['Active Deck Index']=1;prefs['Active Chip Index']=1
  prefs.stats_numTournamentsPlayed=0;prefs.stats_currentBalance=0
  persistent.sGameType=kMode_TexasHoldem
  neutral()
  trace('begin clean_holdem standard_table difficulty=easy campaign_writes=blocked')
end
local savePrefs=SavePrefs
local function restorePrefs() restore(GetPreferences(),savedPrefs,userSetting) end
local function restorePersistent()
  restore(AgentFind('logic_persistent').mProps,savedPersistent)
end
local function restorePhase(phase,fn)
  trace('restore_begin phase='..phase)
  local ok,err=pcall(fn)
  if not ok then trace('restore_failed phase='..phase..' error='..tostring(err)) end
  return ok
end
local function endSession(shuttingDown)
  if not active then return true end
  if not restorePhase('preferences',restorePrefs) then return false end
  -- The native exit destroys the scene and serializes only preferences.
  if not shuttingDown and not restorePhase('persistent',restorePersistent) then return false end
  protectPreferences('0')
  active=false;castOnly=false;savedPrefs=nil;savedPersistent=nil
  prefs=nil;persistent=nil;watchedTable=nil
  trace('end campaign_profile_restored')
  local flush=pendingSave and not shuttingDown
  pendingSave=false
  if flush then savePrefs() end
  return true
end
function COOP_SessionEnd() return endSession(false) end
SavePrefs=function(...) if active then pendingSave=true;return end;return savePrefs(...) end
local saveGame=SaveGame
SaveGame=function(...) if active then return end;return saveGame(...) end
local loadScript=LoadScript
LoadScript=function(...) if COOP_SessionEnd() then return loadScript(...) end end
local quit=EngineQuit
EngineQuit=function(...) if active then COOP_SessionEnd() end;return quit(...) end
-- Native window close does not call the Lua EngineQuit global. The engine
-- invokes this named callback before scene teardown and its own SavePrefs.
-- RegisterShutdownCallback takes a global name, not a Lua function value.
function COOP_SessionShutdown()
  if active then trace('shutdown restoring_campaign_profile');endSession(true) end
end
RegisterShutdownCallback('COOP_SessionShutdown')
local reset=ResetGameStateProperties
ResetGameStateProperties=function(...)
  local result=reset(...)
  if active then neutral() end
  return result
end
local rules=RulesExecute
RulesExecute=function(name,...)
  if active and type(name)=='string' and name:lower()=='bounty_ingameui.rules' then hideChallenges();return end
  return rules(name,...)
end
local initialize=GameTable.Initialize
GameTable.Initialize=function(self,...)
  local result=initialize(self,...)
  if active then self.disableSaves=true end
  return result
end
local initializeAgents=GameTable.InitializeAgents
local intro=Dialogue.PlayIntroCutscene
Dialogue.PlayIntroCutscene=function(self,...)
  if active then
    -- The authored short graph stops the looping limo/menu choreography and
    -- establishes the table camera. host_postBuyIn alone cannot do this.
    persistent.sIntroNode='host_shortWelcome'
    trace('intro short_authored_transition')
  end
  return intro(self,...)
end
GameTable.InitializeAgents=function(self,...)
  if active then
    -- This also covers the original first-ever-play branch. Do not fake the
    -- persistent completed-tournament count or run an uninitialized first deal.
    prefs.stats_numTournamentsPlayed=0
    prefs['Episode - Save Is Valid']=false
    persistent.sGameType=kMode_TexasHoldem
    GameObject.skipOpeningSequence=false
    neutral()
  end
  -- Original code may Yield. Never wrap it in pcall in PN2's Lua 5.1 VM.
  local result=initializeAgents(self,...)
  if COOP_CastOnly() then
    for seat=1,5 do
      local player=self.players[seat]
      local stack=seat==5 and 0 or 25000
      player:SetChips(stack)
      player.previousChips=stack
      player.last5HandChipcount={stack,stack,stack,stack,stack}
    end
    local inactive=self.players[5]
    inactive.folded=true
    inactive.isInThisHand=false
    Logic.bBustedPlayer=true
    Logic.iNumNonBustedPlayers=4
    self:UpdateChipUI(true)
    trace('four_player_cast stacks=25000,25000,25000,25000,0')
  end
  if active then self.disableSaves=true;neutral();hideChallenges();trace('table_initialized intro=short_authored bounties=off') end
  return result
end
local chipUI,roundOrder=GameTable.UpdateChipUI,GameTable.SetRoundOrder
GameTable.UpdateChipUI=function(self,...)
  local result=chipUI(self,...)
  if COOP_CastOnly() then
    for _,name in ipairs({'txt_status_Player','txt_chipcount_Player'}) do
      if AgentExists(name) then AgentHide(name,true) end
    end
  end
  return result
end
GameTable.SetRoundOrder=function(self,state,...)
  if not COOP_CastOnly() then return roundOrder(self,state,...) end
  local seats=self:GetNonBustedPlayers()
  local dealer=self:GetDealer()
  local index=1
  for i,player in ipairs(seats) do if player==dealer then index=i;break end end
  local offset=state.name=='ModePreFlop' and (#seats==2 and 0 or 3) or 1
  self.playerRoundOrder={}
  for i=0,#seats-1 do
    local player=seats[(index-1+offset+i)%#seats+1]
    if not player:IsFolded() then self.playerRoundOrder[#self.playerRoundOrder+1]=player end
  end
end
local tableSave=GameTable.SaveGame
GameTable.SaveGame=function(self,...) if not active then return tableSave(self,...) end end

-- ResolvePot plays the host elimination line but omits cast departures when
-- the host busts on the same hand. Finalize those actors as the shipped
-- WatchBustedAdvs helper does, without enabling the solo simulation flag.
local bustedIdles={
  'adv_brock_bustedOutBooth.chore','adv_claptrap_bustedoutwalkingaround.chore',
  'adv_ash_bustedOutAtBar.chore','adv_samBusted_sittingTable.chore'
}
local function placeRemainingBustedActors(game)
  for seat,idle in ipairs(bustedIdles) do
    local player=game.players[seat]
    local key='bPlayer'..seat..'LeftTable'
    if player:IsBusted() and not Logic[key] then
      local name=player:GetName()
      Logic[key]=true
      AgentSetProperty(name..'_busted','Walk Animation - Idle',idle)
      AgentHide(name..'_busted',false)
      AgentHide(name,true)
      AgentSetProperty(name,'Walk Animation - Idle','')
      if seat==1 then
        for _,prop in ipairs({'obj_knifeBrock','obj_sheathBrock'}) do
          AgentHide(prop,true)
          AgentHide(prop..'_busted',false)
        end
      elseif seat==2 and Themes_GetCurrentTheme()==2 then
        AgentHide('obj_claptrapBanker',true)
        AgentHide('obj_claptrapBanker_busted',false)
      elseif seat==4 then
        AgentHide('Max',false)
        AgentSetProperty('Max','Walk Animation - Idle','adv_samBusted_max_sittingTable.chore')
      end
    end
  end
end
local endTournament=CelebrityPoker_RunEndTournament
CelebrityPoker_RunEndTournament=function(openChore,closeChore,...)
  if active and openChore=='ui_statistics_endoftournament_openMidround.chore' and
     closeChore=='ui_statistics_endoftournament_closeMidround.chore' then
    local game=GameObject:GetTable()
    placeRemainingBustedActors(game)
    if watchedTable~=game then
      watchedTable=game
      trace('host_spectating hand='..game.handNumber)
    end
    return
  end
  return endTournament(openChore,closeChore,...)
end

-- Once The Player busts, ModePreFlop switches to its sequential spectator
-- sweep. CheckHoleCards(true, true) starts a camera chore and exposes the
-- cast member's hole-card UI. Use the ordinary non-revealing animation.
local checkHoleCards=AIPlayer.CheckHoleCards
AIPlayer.CheckHoleCards=function(self,showCamera,wait,...)
  if active and showCamera then return checkHoleCards(self,false,false,...) end
  return checkHoleCards(self,showCamera,wait,...)
end

local areActive=TableBets_AreActive
TableBets_AreActive=function(...) if active then return false end;return areActive(...) end
-- Asset scripts also invoke these entry points; hiding their UI alone is not
-- sufficient to stop progression, modal popups or reward side effects.
local function suppress(owner,name)
  local original=owner[name]
  if type(original)~='function' then return end
  owner[name]=function(...) if not active then return original(...) end end
end
for _,name in ipairs({'TableBets_Activate','TableBets_Open','TableBets_ClearAll','TableBets_KeyTriggered',
  'PlatformUnlockAchievement','Drinks_StartTutorial','HumanPlayerUI_SlideMainToBounty','HumanPlayerUI_OnButtonTriggerRDown',
  'InGameMenu_BountyUnlock','InGameMenu_BountyAwardNotify'}) do suppress(_G,name) end
for _,name in ipairs({'UnlockSpecialItem','ShowSpecialItem','LoseSpecialItem'}) do suppress(Game,name) end
COOP_SessionInstalled=true
