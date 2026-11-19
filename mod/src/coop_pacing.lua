-- Host-authoritative progression, separate from randomized presentation.
-- No engine graph replay or delay estimates. Pure Lua calls preserve yields.
if not (COOP_TestMode and COOP_TestMode()) or COOP_PacingInstalled then return end
assert(COOP_ExchangeInstalled and COOP_ExchangeSync, 'exchange must install first')
assert(GameTable and AIPlayer and HumanPlayer and Player and ModeFlop and PostFlopDealing,
       'pacing classes not loaded yet')
-- Preflight before any mutation, so the native install retry is idempotent
-- even when a class is still being loaded on one peer.
for _,spec in ipairs({{GameTable,'DoRoundState'},{GameTable,'RunAllInBeforeRiverShowCardsChore'},
  {GameTable,'ResolvePot'},{GameTable,'EndRound'},{AIPlayer,'EvaluateAndAct'},
  {HumanPlayer,'EvaluateAndAct'},{AIPlayer,'EndTurn'},{HumanPlayer,'EndTurn'},
  {AIPlayer,'Fold'},{HumanPlayer,'Fold'},{Player,'HandleWin'},{ModeFlop,'Deal'},{PostFlopDealing,'Deal'},{Dialogue,'PlayLine'}}) do
  assert(type(spec[1][spec[2]])=='function','pacing method not loaded: '..spec[2])
end
local guest = COOP_IsGuest and COOP_IsGuest()
local unpackValues = unpack or table.unpack
local counters, pending = {}, {}
local pendingCount, generation = 0, 0
local wrapped = setmetatable({}, {__mode='k'})
local function pack(...) return {n=select('#',...), ...} end
local function trace(s) Print('COOP_PACE ' .. s) end
local function fault(reason)
  trace('FAULT ' .. reason)
  COOP_ExchangeAbort()
  error('Co-op progression stopped: ' .. reason)
  return false
end
COOP_PaceReset = function()
  counters, pending = {}, {}
  pendingCount = 0
  generation = generation + 1
  trace('reset generation=' .. generation)
end
COOP_PaceReceive = function(kind, key)
  local state = pending[key]
  if kind == 3 then
    if state then return fault('duplicate_gate') end
    if pendingCount >= 256 then return fault('gate_overflow') end
    pending[key] = 1; pendingCount = pendingCount + 1
  elseif kind == 4 then
    if state ~= 1 then return fault('release_without_enter') end
    pending[key] = 2
  else return fault('invalid_gate_kind') end
end
local function checkSession(epoch, version)
  local ready, current = COOP_ExchangeSync()
  if not ready or current ~= epoch or generation ~= version then
    return fault('session_changed')
  end
  return true
end
local function waitFor(key, phase, epoch, version)
  local announced = false
  while (pending[key] or 0) < phase do
    if not checkSession(epoch,version) then return false end
    if not announced then trace('wait phase=' .. phase .. ' key=' .. key); announced=true end
    Yield()
  end
  return checkSession(epoch,version)
end
local function emit(phase,key,epoch,version)
  if not checkSession(epoch,version) then return false end
  if COOP_ExchangeEmit(phase,'pace','0','-',key) ~= 1 then return fault('gate_send_failed') end
  return true
end
local function tableFor(self)
  return (self and self.parentTable) or (GameObject and GameObject:GetTable())
end
local function wrap(owner,name,label,detail,predicate)
  assert(type(owner[name]) == 'function', 'missing pacing method ' .. label)
  wrapped[owner] = wrapped[owner] or {}
  if wrapped[owner][name] then return end
  local original=owner[name]
  owner[name]=function(self,...)
    if not (Logic and Logic.bInGame == true) or (predicate and not predicate(self,...)) then
      return original(self,...)
    end
    local args=pack(...)
    local enabled,epoch=COOP_ExchangeSync()
    -- Initial table entry may precede the readiness handshake. Once a call
    -- starts, reset/disconnect cancels it rather than silently releasing it.
    local waitingGeneration=generation
    while not enabled do
      if generation ~= waitingGeneration then fault('session_changed_before_enter'); return end
      Yield()
      if not (Logic and Logic.bInGame == true) then fault('left_gameplay_before_enter'); return end
      enabled,epoch=COOP_ExchangeSync()
      -- Establishing the first session is allowed to change generation.
      if enabled then break end
    end
    local version=generation
    local game=tableFor(self)
    local hand=(game and game.handNumber) or Logic.iHandNumber
    if label:match('^Table%.') then hand=self.handNumber end
    if type(hand) ~= 'number' then fault('missing_hand_context'); return end
    local extra=detail and detail(self,unpackValues(args,1,args.n)) or '-'
    local lane=tostring(hand)..'.'..tostring(Logic.iPokerState or 0)..'.'..label..'.'..extra
    counters[lane]=(counters[lane] or 0)+1
    local key=lane..'.'..counters[lane]
    if #key >= 128 or key:find('[^%w%._%-]') then fault('invalid_gate_identity'); return end
    if guest then
      if not waitFor(key,1,epoch,version) then return end
    elseif not emit('gate_enter',key,epoch,version) then return end
    trace('enter epoch='..epoch..' key='..key)
    -- Do not pcall: Lua 5.1 engine calls can yield. An original error must
    -- propagate and must never be converted to a successful host release.
    local result=pack(original(self,unpackValues(args,1,args.n)))
    if guest then
      if not waitFor(key,2,epoch,version) then return end
      pending[key]=nil; pendingCount=pendingCount-1
    elseif not emit('gate_release',key,epoch,version) then return end
    trace('complete epoch='..epoch..' key='..key)
    return unpackValues(result,1,result.n)
  end
  wrapped[owner][name]=true
end
local function seat(self) return tostring(self.index or 5) end
local function state(self,mode) return mode.name end
-- These operations exist on both peers even when optional dialogue differs.
wrap(GameTable,'DoRoundState','Table.Round',state)
wrap(GameTable,'RunAllInBeforeRiverShowCardsChore','Table.Reveal')
wrap(GameTable,'ResolvePot','Table.Resolve')
wrap(GameTable,'EndRound','Table.End')
wrap(AIPlayer,'EvaluateAndAct','AI.Decide',seat)
wrap(HumanPlayer,'EvaluateAndAct','Human.Decide',seat)
wrap(AIPlayer,'EndTurn','AI.EndTurn',seat)
wrap(HumanPlayer,'EndTurn','Human.EndTurn',seat)
-- Fold mutates state and starts its own transition/idle thread BEFORE EndTurn.
-- Hold that mutation until the host has finished its action dialogue, rather
-- than allowing an early local fold alongside the still-playing exchange.
wrap(AIPlayer,'Fold','AI.Fold',seat)
-- Gate each concrete player method. Leave Player.Fold alone: both concrete
-- methods call that base mutation inside their already-held gate.
wrap(HumanPlayer,'Fold','Human.Fold',seat)
wrap(Player,'HandleWin','Player.Win',seat)
wrap(ModeFlop,'Deal','Deal.Flop')
wrap(PostFlopDealing,'Deal','Deal.PostFlop')
-- Known deterministic graph calls: do not pair randomized reactions,
-- banter iterations, optional considers, or process-local DlgWait IDs.
local stableLines={dealer_hands=true,dealer_handWinner=true,
  dealer_playerEliminated=true,host_tournamentWinner=true,showdownZoomIn=true}
-- RunBlindRaise enables Deal after PlayLine returns. Hold that local input
-- until the host release has passed the guest's finite exit in the replay queue.
stableLines.dealer_blinds=COOP_HandReadyInstalled==true
wrap(Dialogue,'PlayLine','Dialogue.Line',function(self,dlg,line) return line end,
  function(self,dlg,line)
    return stableLines[line] and
      ((dlg=='dealer.dlog' and line:match('^dealer_')) or
       (dlg=='showdown.dlog' and line=='showdownZoomIn') or
       (line=='host_tournamentWinner' and dlg==kHostDlg))
  end)
COOP_PacingInstalled=true
trace('installed version=4 role='..(guest and 'guest' or 'host'))
