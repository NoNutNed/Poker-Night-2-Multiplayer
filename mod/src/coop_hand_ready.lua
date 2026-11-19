-- Keep the authored RunTournament, Deal and blind-raise UI. Only the
-- completed-hand barrier can release the game loop after the first hand.
if COOP_HandReadyInstalled or not COOP_HandReadyAction then return end
assert(Game and Game.DoRound and Game.OnMouseL and Game.OnMouseLUp and Game.OnDoRoundButtonPress and Game.SetTournamentPlaying,
       'hand-ready game methods unavailable')
-- The lobby is committed before this chunk loads. The legacy COOP_IsGuest
-- binding is registered later in the same native installer.
local _,phase,role=COOP_LobbyStatus()
assert(phase==3 and (role==0 or role==1),'hand-ready lobby role unavailable')
local guest=role==1
local doRound,mouseDown,mouseUp,buttonPress=Game.DoRound,Game.OnMouseL,Game.OnMouseLUp,Game.OnDoRoundButtonPress
local setPlaying=Game.SetTournamentPlaying
local skip=CelebrityPoker_SkipToNextHand
local boundary
local dealButton='btn_doRound'
local skipButton='ui_player_buttonFoldSkip'
local skipGroup='ui_player_buttonFoldSkipParent'
local statusAgent='coop_between_hands_status'
local seatNames={'Brock','Claptrap','Ash','Sam'}
local function active()
  return COOP_SessionActive() and Logic and Logic.bInGame==true
end
local function trace(message)
  Print('COOP_HAND_READY '..message)
end
local function showStatus(text)
  if not text then
    if AgentExists(statusAgent) then AgentHide(statusAgent,true) end
    return
  end
  if not AgentExists(dealButton) then return end
  -- The overlay and blind-popup cameras share the authored Deal plane.
  -- Place a separate text agent above the button, outside the card/action HUD.
  local pos=AgentGetPos(dealButton)
  if not pos then return end
  if not AgentExists(statusAgent) then
    AgentCreate(statusAgent,'module_text.prop',Vector(pos.x,pos.y+0.7,pos.z),nil,kUIScene,false,false)
    AgentSetProperty(statusAgent,'Text Font','Futura_LT_Book_24.font')
    AgentSetProperty(statusAgent,'Text Alignment Horizontal','Centered')
    AgentSetProperty(statusAgent,'Text Alignment Vertical','Top')
    AgentSetProperty(statusAgent,'Text Scale',0.7)
    AgentSetProperty(statusAgent,'Text Background',true)
    AgentSetProperty(statusAgent,'Text Background Color',{r=0.02,g=0.025,b=0.04,a=0.9})
    AgentSetProperty(statusAgent,'Text Render Layer',29000)
  end
  AgentSetPos(statusAgent,Vector(pos.x,pos.y+0.7,pos.z))
  AgentSetProperty(statusAgent,'Text String',text)
  AgentHide(statusAgent,false)
end
local function statusText(game,votes)
  local _,_,_,peer,_,_,guests,_,_,s2,s3,s4,s5=COOP_LobbyStatus()
  local seats={s2,s3,s4,s5}
  local count,pending=0,{}
  for i=0,guests do
    if math.floor(votes/2^i)%2==1 then count=count+1
    else pending[#pending+1]=i==0 and 'Host' or seatNames[seats[i]] end
  end
  local owned=guest and seats[peer-1] or 5
  local player=game:GetTable().players[owned]
  local prefix=player and player:IsBusted() and 'Spectating  |  ' or ''
  local tail=#pending>0 and '  |  Waiting for '..table.concat(pending,', ') or ''
  return prefix..'Ready '..count..'/'..(guests+1)..tail
end
local function vote(game)
  local gate=boundary
  if not gate or gate.game~=game or gate.requested or gate.failed or IsPaused() then return end
  if not AgentExists(dealButton) or AgentIsHidden(dealButton) then return end
  local _,_,_,_,focused=COOP_LobbyInput('focus')
  local button=AgentFind(dealButton)
  if focused~=1 or not button.mProps['Game Selectable'] then return end
  gate.requested=true
  button.mProps['Game Selectable']=false
  trace('local_vote hand='..gate.hand)
end
Game.OnMouseL=function(self,button,...)
  if active() and AgentGetName(button)==skipButton then return end
  return mouseDown(self,button,...)
end
Game.OnMouseLUp=function(self,button,...)
  if active() then
    local name=AgentGetName(button)
    if name==skipButton then return end
    if name==dealButton then return vote(self) end
  end
  return mouseUp(self,button,...)
end
Game.OnDoRoundButtonPress=function(self,...)
  if active() then return vote(self) end
  return buttonPress(self,...)
end
CelebrityPoker_SkipToNextHand=function(...)
  if not active() then return skip(...) end
end
ThreadStart(function()
  while true do
    if active() and AgentExists(skipButton) then
      local button=AgentFind(skipButton)
      if button.mProps['Game Selectable'] then button.mProps['Game Selectable']=false end
      -- Fold chores animate this group directly in both poker modes.
      if AgentExists(skipGroup) and not AgentIsHidden(skipGroup) then
        AgentHide(skipGroup,true,true)
      end
      if not AgentIsHidden(skipButton) then AgentHide(skipButton,true,true) end
    end
    Yield()
  end
end)
local function stop(gate,reason)
  gate.failed=true
  trace('stopped hand='..gate.hand..' reason='..reason)
  COOP_ExchangeAbort()
  showStatus('Multiplayer session ended')
end
local function monitor(gate)
  local lastVotes,lastText
  while active() and boundary==gate do
    local live,hand,votes,required=COOP_HandReadyStatus()
    if live~=1 or hand>gate.hand then return stop(gate,'session_ended') end
    local paused=IsPaused()
    local text
    if not gate.released and not paused and AgentExists(dealButton) and not AgentIsHidden(dealButton) then
      text=statusText(gate.game,hand==gate.hand and votes or 0)
    end
    if text~=lastText then
      showStatus(text)
      lastText=text
    end
    if hand==gate.hand then
      if votes~=lastVotes then
        trace('status hand='..hand..' votes='..votes..' required='..required)
        lastVotes=votes
      end
      if gate.requested and not gate.submitted then
        if COOP_HandReadyAction('vote',gate.hand)~=1 then return stop(gate,'vote_failed') end
        gate.submitted=true
      end
      if gate.submitted and votes==required and not paused then
        if not gate.released then
          gate.released=true
          showStatus()
          trace('released hand='..hand)
        end
        -- The original DoRound still owns the blind-popup and Deal close.
        -- The tournament can reset its local playing bit after a yielding
        -- blind announcement. Hold the release until DoRound consumes it.
        setPlaying(gate.game,true)
      end
    end
    Yield()
  end
  if boundary==gate then showStatus() end
end
Game.DoRound=function(self,...)
  if not active() then return doRound(self,...) end
  local previous=self:GetTable().handNumber
  if previous>0 then
    local gate=boundary
    assert(gate and gate.game==self and gate.hand==previous,'missing completed-hand readiness')
    while active() and not gate.failed and not gate.released do Yield() end
    if not active() or gate.failed then error('Multiplayer ended while waiting to deal') end
  end
  boundary=nil
  doRound(self,...)
  if active() and not self:IsGameOver() then
    local poker=self:GetTable()
    local nextHand=poker.handNumber+1
    if nextHand>1 and nextHand%poker.blindPeriod==1 and AgentExists(dealButton) then
      -- RunBlindRaise re-enables the button when its announcement finishes.
      AgentFind(dealButton).mProps['Game Selectable']=false
    end
    local gate={game=self,hand=poker.handNumber}
    boundary=gate
    if not guest and COOP_HandReadyAction('open',gate.hand)~=1 then
      return stop(gate,'open_failed')
    end
    ThreadStart(monitor,gate)
  end
end
COOP_HandReadyInstalled=true
trace('installed role='..(guest and 'guest' or 'host'))
