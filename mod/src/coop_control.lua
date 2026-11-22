-- Opt-in guest input, still using the accepted full-deck replica for this slice.
if not (COOP_TestMode and COOP_TestMode()) or COOP_ControlInstalled or
   not COOP_ControlOptions or COOP_ControlOptions()~=1 then return end
assert(AIPlayer and HumanPlayer and Player and GameTable and type(AIPlayer.ShowTells)=='function', 'control classes unavailable')
assert(type(GameTable.InitRound)=='function' and type(AIPlayer.EvaluateAndAct)=='function' and
       type(HumanPlayer.EvaluateAndAct)=='function', 'control methods unavailable')
local guest=COOP_IsGuest and COOP_IsGuest()
local multi=COOP_MultiOptions and COOP_MultiOptions()==1
local cast,hostCastSeat
if COOP_LobbyStatus then cast,hostCastSeat=select(15,COOP_LobbyStatus()) end
cast=cast==1
local function multiRoster()
  local _,localSeat,epoch,revision,s2,s3,active,held,rejected,s4,s5=COOP_MultiOptions()
  local seats={}
  for _,seat in ipairs({s2 or 0,s3 or 0,s4 or 0,s5 or 0}) do if seat>0 then seats[#seats+1]=seat end end
  if cast then
    seats[#seats+1]=hostCastSeat
    if not guest then localSeat=hostCastSeat end
  end
  return localSeat,epoch,revision,seats,active
end
if guest and COOP_SeatPickerOptions and COOP_SeatPickerOptions()==1 then
  assert(type(DialogBox)=='function' and type(COOP_SeatPickerChoose)=='function', 'seat picker UI unavailable')
end
local streets={ModePreFlop=1,ModeFlop=2,ModeTurn=3,ModeRiver=4}
local context, ui
local unpackValues=unpack or table.unpack
local function pack(...) return {n=select('#',...),...} end
local function trace(s) Print('COOP_CONTROL '..s) end
local function fault(s)
  ui=nil; trace('FAULT '..s); COOP_ExchangeAbort(); error('Co-op controls stopped: '..s)
end
local function check()
  local enabled,seat,epoch,revision=COOP_ControlOptions()
  local validSeat=context and seat==context.seat
  if multi and context then
    local localSeat,e,r,seats,active=multiRoster()
    local count=0;for _ in pairs(context.owned) do count=count+1 end
    validSeat=active==1 and localSeat==context.localSeat and #seats==count and e==epoch and r==revision
    for _,owned in ipairs(seats) do if not context.owned[owned] then validSeat=false end end
  end
  if not context or enabled~=1 or not validSeat or epoch~=context.epoch or revision~=context.revision or
     not (Logic and Logic.bInGame==true) then fault('ownership_or_connection_changed') end
end
COOP_HumanOwns=function(player)
  return context and player and player.parentTable==context.game and
    (multi and context.owned[player.index] or player.index==context.seat)
end
COOP_ControlLocalPlayer=function(game)
  if (guest or cast) and context and context.game==game then return game.players[context.localSeat or context.seat] end
end
COOP_EmoteTable=function() return context and context.game end
COOP_RequestCastEmote=function(game,mode,index)
  local player=COOP_ControlLocalPlayer(game)
  if not player or not COOP_HumanOwns(player) or player:IsBusted() then return false end
  if not guest then return COOP_ApproveLocalEmote and COOP_ApproveLocalEmote(player,game,mode,index) end
  local line=string.format('CSEAT epoch=%d rev=%d seat=%d hand=%d street=0 turn=0 kind=5 action=%d amount=%d',
    context.epoch,context.revision,player.index,game.handNumber,mode,index)
  return COOP_ControlSend(line)==1
end
local function send(kind,turn,state,action,amount)
  check()
  local c=context
  local line=string.format('CSEAT epoch=%d rev=%d seat=%d hand=%d street=%d turn=%d kind=%d action=%d amount=%d',
    c.epoch,c.revision,c.seat,c.game.handNumber,streets[state.name],turn,kind,action or 0,amount or 0)
  if COOP_ControlSend(line)~=1 then fault('send_failed') end
end
local function take(turn,state)
  check()
  local k,e,r,s,h,street,t,a,n=COOP_ControlTake()
  if not k then return end
  if e~=context.epoch or r~=context.revision or s~=context.seat or h~=context.game.handNumber or
     street~=streets[state.name] or t~=turn then
    trace('reject stale_or_wrong_turn'); return
  end
  return k,a,n
end
local function streetState(game,state)
  if not streets[state.name] then fault('unsupported_street') end
  if context.street~=state.name then
    context.street=state.name; context.minRaise=game:GetBigBlindValue()
    context.lastTarget=nil; context.lastMin=nil
    if multi then context.history={} end
  end
end
local function legal(self,action,amount)
  if type(amount)~='number' or amount%1~=0 or amount<0 or amount>2147483647 then return false end
  local game=self.parentTable
  local call=math.max(0,game:GetCallValue()-self:GetChipsInPot())
  local chips=self:GetChips()
  if self:IsFolded() or self:IsAllIn() or chips<=0 then return false end
  if action==3 then return amount==0 end
  if action==4 then return call==0 and amount==0 end
  if action==1 then return call>0 and amount==math.min(call,chips) end
  if action~=2 or amount<=call or amount>chips then return false end
  local previous=multi and context.history[self.index] or context
  if previous and previous.lastTarget and game:GetCallValue()-previous.lastTarget<(previous.lastMin or context.minRaise) then return false end
  local others=false
  for _,p in ipairs(game:GetActivePlayers()) do
    if p~=self and not p:IsAllIn() then others=true end
  end
  if not others then return false end
  return amount-call>=context.minRaise or amount==chips
end
COOP_ControlLegal=legal -- offline seam; never grants ownership or mutates chips.

-- Public action flags from AIPlayer.SetDialogLogic. No hand-informed bad-hole
-- classification or AI ConsiderBet for a human-controlled cast character.
local function publicEffects(self,state,action,astr,amount,call)
  local g=self.parentTable
  local function flag(key) if key and self.dialogLogicKeys and self.dialogLogicKeys[key] then Logic[self.dialogLogicKeys[key]]=true end end
  local bb=g:GetBigBlindValue()
  if action==2 then
    if amount-call>=Logic.iChipcount_Player and not Logic.bIfPlayerCallsHeIsAllInHasPlayed and
       not g.players[5]:IsBusted() and not g.players[5]:IsFolded() and not g.players[5]:IsAllIn() then Logic.bIfPlayerCallsHeIsAllIn=true end
    if astr==kAction_Bet then
      if amount<bb*3 then flag(kBetSmallBet) end
      if amount>=bb*6 then flag(kBetBigBet) end
    elseif astr==kAction_Raise then
      if amount<bb*3 then flag(kRaiseSmallRaise) end
      if amount>=bb*6 then flag(kRaiseBigRaise) end
      local count=0
      for _,p in ipairs(g:GetPlayers()) do if p~=self and p.raisedThisRound then count=count+1 end end
      if count>=2 then flag(kRaiseReRaise) end
    end
  elseif action==1 then
    if g:NumRaisesThisRound()>0 then flag(amount<bb*6 and kCallSmallBet or kCallBigBet) end
    if g:GetPosition(self)==#g:GetActivePlayers() then flag(astr==kAction_Check and kCheckLastInRound or kCallLastInRound) end
  elseif action==3 then
    if #g:GetActivePlayers()==2 then flag(kFoldFoldsToWinner); Logic.sDontFoldIdlePlayer=self:GetName()
    elseif #g:GetActivePlayers()>2 and state.name~='ModePreFlop' then
      local count=0
      for _,p in ipairs(g:GetActivePlayers()) do if p~=self and p.actedThisRound then count=count+1 end end
      if count==2 then flag(kFoldTooManyPlayers) end
    end
  end
  if action==1 or action==2 then flag(amount<=1200 and kAllInSmall or kAllInBig) end
  self.justPlayedBadHoleConsec=false
  if not Logic.bModeInstant and amount>0 and kChipValues and amount<=kChipValues[1]*6 then
    local remap={}
    for i=6,1,-1 do
      if amount<i*kChipValues[1] then
        remap[#remap+1]='obj_chipBetPlayer'..self.index..'_0'..i;remap[#remap+1]='__skip'
      else break end
    end
    DlgSetExchangeChoreAgentRemap(unpackValues(remap))
  end
end
local function apply(self,state,action,amount)
  local g=self.parentTable
  local call=math.max(0,g:GetCallValue()-self:GetChipsInPot())
  self:ClearDialogLogicKeys()
  Logic['sPlayer'..self.index..'ConsiderTell']=''
  self.onBluff=false; self.onCheckRaise=false
  local astr
  if action==1 or action==4 then
    local paid=self:Call()
    if paid~=amount then fault('call_contribution_mismatch') end
    astr=paid==0 and kAction_Check or (self:GetChips()==0 and kAction_AllIn or kAction_Call)
    action=1
  elseif action==2 then
    local old=self:GetChips()
    self:Raise(amount-call)
    if old-self:GetChips()~=amount then fault('raise_contribution_mismatch') end
    astr=self:GetChips()==0 and kAction_AllIn or (call==0 and state.name~='ModePreFlop' and kAction_Bet or kAction_Raise)
    if state.name=='ModePreFlop' then self.raisedPreFlop=true end
  else
    astr=kAction_Fold
  end
  Logic.iLastBetAmount=action==3 and -1 or amount
  Logic.bCalledAllIn=(call>=self:GetChips()+amount and amount>0 and action==1)
  -- Select ordinary public poker response flags before removing folded seat,
  -- matching original SetDialogLogic's active-player exclusions (not folded yet).
  publicEffects(self,state,action,astr,amount,call)
  if action==3 then
    local active=g:GetActivePlayers()
    for i,p in ipairs(active) do if p==self then table.remove(active,i);break end end
    ShowdownResponseManager_GetWinningPercentages(active)
  end
  Logic['LastAction_Player'..self.index]=astr;self.playerAction=action
  trace('applied hand='..g.handNumber..' turn='..context.turn..' seat='..self.index..' action='..action..' amount='..amount..' chips='..self:GetChips())
  return action,astr
end

local function refreshUI()
  if not ui then return end
  local self=ui.player
  local call=math.max(0,self.parentTable:GetCallValue()-self:GetChipsInPot())
  AgentSetProperty('btn_call','Text Dialog 2.0 Node Name',call==0 and 'btn_check' or 'btn_call')
  AgentSetProperty('btn_raise','Text Dialog 2.0 Node Name',ui.amount==self:GetChips() and 'btn_allin' or (call==0 and 'btn_bet' or 'btn_raise'))
  -- Card faces use coop_card_ui.lua; the shared host action badge is untouched.
  AgentSetProperty('txt_raiseAmount',kPropertyTxtStr,tostring(ui.amount-call))
  for _,button in ipairs({'btn_fold','btn_call','btn_raise'}) do AgentSetSelectable(button,not ui.sent) end
end
local function submit(action)
  if not ui or ui.sent then return end
  check()
  local p=ui.player
  local call=math.max(0,p.parentTable:GetCallValue()-p:GetChipsInPot())
  if action==1 and call==0 then action=4 end
  local amount=action==2 and ui.amount or (action==1 and math.min(call,p:GetChips()) or 0)
  if not legal(p,action,amount) then trace('ui_illegal_action');return end
  if COOP_EmoteClose then COOP_EmoteClose() end
  if guest then send(2,ui.turn,ui.state,action,amount)
  else ui.pending={action,amount} end
  ui.sent=true;refreshUI()
end
local function openDecision(player,state,turn)
  local call=math.max(0,player.parentTable:GetCallValue()-player:GetChipsInPot())
  ui={player=player,state=state,turn=turn,amount=math.min(player:GetChips(),call+context.minRaise)}
  ChorePlayAndWait('ui_status_Player_activate.chore')
  if Input_UseCursor() then CursorHide(false) end
  ConsoleInput_SetInputEnabled(true);InputMapperActivate('celebrityPoker.imap')
  HumanPlayerUI_ResetSlider();refreshUI()
end
local function setAmount(value)
  if not ui or ui.sent then return end
  local p=ui.player
  local call=math.max(0,p.parentTable:GetCallValue()-p:GetChipsInPot())
  ui.amount=math.max(math.min(p:GetChips(),call+context.minRaise),math.min(p:GetChips(),math.floor(value/100)*100))
  refreshUI()
end
local function pickSeat(self)
  if not guest or not COOP_SeatPickerOptions then return end
  local enabled,chosen,initial=COOP_SeatPickerOptions()
  if enabled~=1 or chosen>0 then return end
  if self.handNumber~=0 then fault('picker_requires_fresh_table') end
  local names={'Brock','Claptrap','Ash','Sam'}
  local seat=names[initial] and initial or 1
  local generation
  local function healthy()
    local current,connected,failed=COOP_ExchangeConnection()
    if not (Logic and Logic.bInGame==true) or type(current)~='number' or failed~=0 or
       (generation and (current~=generation or connected~=1)) then fault('picker_connection_changed') end
    if connected==1 then generation=current end
  end
  healthy()
  trace('picker_open')
  local prompt='Choose your character for this session. The host plays The Player.\nRestart the guest to choose again.'
  while true do
    while DialogBox_IsActive and DialogBox_IsActive() do healthy();Yield() end
    -- Native generic popup supports exactly three buttons. Keep its normal
    -- open/close, cursor and input cleanup; no pcall across engine yields.
    local result=DialogBox(prompt,
      'Co-op character: '..names[seat],false,nil,'Play as '..names[seat],1,'Next',2,'Previous',3)
    healthy() -- Readiness may bounce during table loading; socket health may not.
    if result==1 then
      if COOP_SeatPickerChoose(tostring(seat))~=1 then fault('picker_selection_failed') end
      trace('picker_selected seat='..seat..' name='..names[seat])
      if not multi then return end
      while true do
        healthy()
        local _,_,_,_,_,_,_,held,rejected=COOP_MultiOptions()
        if held==1 then return end
        if rejected==1 then prompt='That character is already taken. Choose another character.';break end
        Yield()
      end
    elseif result==2 then seat=seat%4+1
    elseif result==3 then seat=(seat+2)%4+1
    else fault('picker_closed_without_selection') end
  end
end
local init=GameTable.InitRound
GameTable.InitRound=function(self,...)
  context=nil;ui=nil
  if self.gameType~=kMode_TexasHoldem then fault('controls_holdem_only') end
  pickSeat(self)
  if multi and not guest then
    -- HAND_START/deck are not replayable snapshots. Do not publish either
    -- until BOTH sockets are present and their roster is acknowledged.
    while true do
      if not (Logic and Logic.bInGame==true) then fault('left_gameplay_before_multi_hand') end
      local mode,seat=COOP_ControlOptions()
      local ready=COOP_ExchangeSync()
      local _,_,failed=COOP_ExchangeConnection()
      if mode~=1 or failed~=0 then fault('multi_session_stopped_before_hand') end
      if ready and seat>0 then break end
      Yield()
    end
  end
  local result=pack(init(self,...))
  while true do
    if not (Logic and Logic.bInGame==true) then fault('left_gameplay_during_claim') end
    local mode,seat,epoch,revision,capable=COOP_ControlOptions()
    local ready=COOP_ExchangeSync()
    if multi then
      local _,_,failed=COOP_ExchangeConnection()
      if failed~=0 then fault('multi_session_stopped_during_claim') end
    end
    if ready and capable~=1 then fault('peer_controls_not_enabled') end
    if mode~=1 then fault('control_mode_changed') end
    if ready and seat>0 then
      context={game=self,seat=seat,epoch=epoch,revision=revision,turn=0}
      local seats={seat}
      if multi then
        local localSeat,_,_,roster,active=multiRoster()
        if active~=1 then fault('multi_roster_not_ready') end
        context.localSeat=localSeat;context.owned={};context.history={}
        seats=roster;for _,owned in ipairs(seats) do context.owned[owned]=true end
      end
      for _,owned in ipairs(seats) do
        self.players[owned]:ClearDialogLogicKeys()
        Logic['sPlayer'..owned..'ConsiderTell']=''
        trace('ownership_active epoch='..epoch..' revision='..revision..' seat='..owned..' hand='..self.handNumber)
      end
      break
    end
    Yield()
  end
  return unpackValues(result,1,result.n)
end
local show=AIPlayer.ShowTells
AIPlayer.ShowTells=function(self,...) if COOP_HumanOwns(self) then return end;return show(self,...) end
local ai=AIPlayer.EvaluateAndAct
AIPlayer.EvaluateAndAct=function(self,state,...)
  if not COOP_HumanOwns(self) then return ai(self,state,...) end
  if multi then context.seat=self.index end
  check(); streetState(self.parentTable,state)
  context.turn=context.turn+1
  local turn=context.turn
  while self.tellChoreController and ControllerIsPlaying(self.tellChoreController) do check();Yield() end
  -- Preserve the original public Big Pot response, with the host as its sole
  -- selector. The selected-exchange stream already carries its voice/chore.
  if not guest and self.parentTable:GetPotManager():GetAmount()>kPlayerStartingChips and
     self.parentTable:NumRaisesThisRound()>1 and RandomInt(0,10)<5 and not Logic.bPlayedBigPotResponse then
    DialogueObject:PlayLine('responses.dlog','Big Pot')
  end
  Player.EvaluateAndAct(self,state)
  if not guest then
    send(1,turn,state)
    local localTurn=cast and context.localSeat==self.index
    if localTurn then openDecision(self,state,turn) end
    while true do
      local kind,action,amount
      if localTurn then
        check()
        if ui.pending then
          kind,action,amount=2,ui.pending[1],ui.pending[2]
          ui.pending=nil
        end
      else kind,action,amount=take(turn,state) end
      if kind then
        if kind~=2 then fault('expected_intent')
        elseif legal(self,action,amount) then
          local a,s=apply(self,state,action,amount)
          if localTurn then ui=nil;ChorePlay('ui_status_Player_deactivate.chore') end
          send(3,turn,state,action,amount)
          return a,s
        else
          trace('reject illegal_intent hand='..self.parentTable.handNumber..' turn='..turn)
          if localTurn then ui.sent=false;refreshUI()
          else send(4,turn,state) end
        end
      end
      Yield()
    end
  end
  local awaiting=true
  while awaiting do
    local kind=take(turn,state)
    if kind then if kind~=1 then fault('expected_turn') end;awaiting=false else Yield() end
  end
  local localTurn=not multi or context.localSeat==self.index
  if localTurn then
    openDecision(self,state,turn)
    trace('awaiting_guest hand='..self.parentTable.handNumber..' turn='..turn..' seat='..self.index)
  end
  while true do
    local kind,action,amount=take(turn,state)
    if kind==4 then if ui then ui.sent=false;refreshUI() end
    elseif kind==3 then
      if not legal(self,action,amount) then fault('result_illegal_in_replica') end
      ui=nil
      if localTurn then ChorePlay('ui_status_Player_deactivate.chore') end
      return apply(self,state,action,amount)
    elseif kind then fault('unexpected_guest_event') end
    Yield()
  end
end
-- Observe public call-target changes from every seat, keeping full-raise size
-- across short all-ins. Reopening uses cumulative target increase since last act.
for _,owner in ipairs({AIPlayer,HumanPlayer}) do
  local original=owner.EvaluateAndAct
  owner.EvaluateAndAct=function(self,state,...)
    if not context or self.parentTable~=context.game or not (Logic and Logic.bInGame==true) then return original(self,state,...) end
    streetState(self.parentTable,state)
    local before=self.parentTable:GetCallValue()
    local result=pack(original(self,state,...))
    local after=self.parentTable:GetCallValue()
    if after-before>=context.minRaise then context.minRaise=after-before end
    if COOP_HumanOwns(self) then
      if multi then context.history[self.index]={lastTarget=after,lastMin=context.minRaise}
      else context.lastTarget=after;context.lastMin=context.minRaise end
    end
    return unpackValues(result,1,result.n)
  end
end
if guest or cast then
  for _,spec in ipairs({{'FoldButton',3},{'CallButton',1},{'RaiseButton',2}}) do
    local action=spec[2]
    HumanPlayer[spec[1]]=function() submit(action) end
  end
  HumanPlayer.PlusButton=function() if ui then setAmount(ui.amount+100) end end
  HumanPlayer.MinusButton=function() if ui then setAmount(ui.amount-100) end end
  HumanPlayer.SetBetValueFromSlider=function(self,value)
    if ui then
      local p=ui.player
      local minimum=math.min(p:GetChips(),math.max(0,p.parentTable:GetCallValue()-p:GetChipsInPot())+context.minRaise)
      setAmount(minimum+(p:GetChips()-minimum)*math.max(0,math.min(1,value)))
    end
  end
end
COOP_ControlInstalled=true
trace('installed role='..(guest and 'guest' or 'host')..' full_deck_intermediate=1')
