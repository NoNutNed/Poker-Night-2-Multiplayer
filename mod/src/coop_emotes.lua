-- Gameplay HUD browser and host-authoritative, hand-scoped cast emotes.
if COOP_EmotesInstalled or not COOP_HandReadyInstalled or not COOP_EmoteCatalog then return end

local guest=COOP_IsGuest()
local catalogs={{},{},{},{}}
local catalogLoaded=false
local modes={'Animation','Voice','Animation + voice'}
local categories={'Browse all','Hurry up','Big bets','Challenges','Trash talk'}
local groups={'_takingtoolong_','_callingotherbigbet_','_callingownbet_','_badhand_'}
local localPlayer,lastSequence,sequence= nil,0,0
local controllers={}
local page,mode,selection,alphabetical=1,3,1,true
local pageModes={}
local query,searching='',false
local open=false
local rows,agents={},{}
local hitTargets,pendingClick={},nil
local consumeRelease=false
local labelCursor
local lines=9

local phrases={
  takingtoolong='Hurry the table',callingotherbigbet='React to a big bet',
  callingownbet='Challenge a call',badhand='Mock a bad hand',
  nodno='Shake head',nodyes='Nod',betbig='Make a big bet',betall='Go all in',
  armoutright='Extend right arm',checkcards='Check cards',considerchips='Consider the chips',
}
local function titleFor(name,voice)
  local label=name:gsub('^(%a+)_%1_',''):gsub('^sk%d+_[%a%d]+_','')
  local action,variant=label:match('^taunts_(%a+)_(%d+)$')
  if phrases[action] then return phrases[action]..' '..variant end
  if phrases[label] then return phrases[label] end
  if label:match('^%d+$') then return (voice~='' and 'Voice line ' or 'Animation ')..label end
  label=label:gsub('^responses_',''):gsub('^taunts_',''):gsub('_',' '):gsub('%.anm$',''):gsub('%.chore$','')
  return label:gsub('^%l',string.upper)
end

local function loadCatalog()
  if catalogLoaded then return end
  local part=0
  while true do
    local data=COOP_EmoteCatalog(tostring(part))
    if not data then break end
    for row in data:gmatch('[^\n]+') do
      local seat,voice,chore,name,animation,metadata=row:match('^([1-4])|([^|]*)|([^|]*)|([^|]+)|([^|]*)(.*)$')
      if seat then
        local entries=catalogs[tonumber(seat)]
        local title=titleFor(name,voice)
        local dialog,node,line=metadata:match('^|([%w_]+%.dlog)|(%d+)|(%d+)$')
        entries[#entries+1]={id=#entries+1,voice=voice,chore=chore,name=name,title=title,animation=animation,
          dialog=dialog,node=node,line=line,search=(title..' '..name:gsub('_',' ')..' '..voice):lower()}
      end
    end
    part=part+1
  end
  catalogLoaded=true
end

local function trace(value) Print('COOP_EMOTE '..value) end
local function gameplay()
  return Logic and Logic.bInGame==true and COOP_SessionActive()
end
local function busy()
  return not gameplay() or Logic.bBanterPlaying or Logic.bPauseBanter or Logic.bPausedPokerActions or
    (DialogueObject and DialogueObject:IsPokerLineRunning()) or
    (guest and COOP_ExchangePresentationBusy and COOP_ExchangePresentationBusy()) or
    (GameObject and (GameObject.runningIntroCutscene or GameObject.tutorialIsRunning)) or
    InGameMenu_IsMenuActive()
end
local function stop()
  for _,controller in ipairs(controllers) do
    local ok,playing=pcall(ControllerIsPlaying,controller)
    if ok and playing then pcall(ControllerKill,controller) end
  end
  controllers={}
end
local function close()
  if not open then return end
  open=false
  searching=false
  hitTargets,pendingClick={},nil
  for _,name in pairs(agents) do if AgentExists(name) then AgentHide(name,true) end end
end
local function owns(player)
  return player and type(player.index)=='number' and player.index>=1 and player.index<=4 and
    COOP_HumanOwns(player) and not player:IsBusted()
end
local function playing()
  for _,controller in ipairs(controllers) do
    local ok,active=pcall(ControllerIsPlaying,controller)
    if ok and active then return true end
  end
  controllers={}
  return false
end
local function playable(entry,kind)
  if not entry then return false end
  if kind==1 then return entry.animation~='' or (entry.voice=='' and entry.chore~='') end
  if kind==2 then return entry.voice~='' end
  return kind==3 and entry.voice~='' and (entry.chore~='' or entry.animation~='')
end
local function start(action,...)
  if type(action)~='function' then return end
  local ok,result=pcall(action,...)
  if ok and result then
    controllers[#controllers+1]=result
    return result
  end
end
local function voice(entry)
  local stem=entry.voice:gsub('%.fsb$','')
  local controller=start(VoicePlay,stem) or start(SoundPlay,entry.voice)
  if controller and ControllerSetVoice then pcall(ControllerSetVoice,controller,true) end
  return controller
end
local function animate(player,entry)
  if entry.animation~='' then return start(PlayAnimation,player:GetName(),entry.animation) end
  return start(ChorePlay,entry.chore)
end
local function play(player,entry,kind,key)
  stop()
  local result
  if kind==1 then result=animate(player,entry)
  elseif kind==2 then result=voice(entry)
  elseif entry.chore~='' then result=start(ChorePlay,entry.chore)
  elseif voice(entry) then result=animate(player,entry) end
  if not result then stop() end
  trace((result and 'play' or 'unavailable')..' seat='..player.index..' key='..key..
    ' mode='..kind..' id='..entry.id..' asset='..entry.name)
  return result~=nil
end

COOP_EmoteClose=close
COOP_EmotePreempt=stop
COOP_EmoteReset=function()
  close();stop()
  localPlayer,lastSequence,sequence=nil,0,0
end

local function approve(seat,hand,kind,id)
  local game=COOP_EmoteTable()
  local player=game and game.players[seat]
  if not game or game.handNumber~=hand or not owns(player) or busy() or playing() then return end
  loadCatalog()
  local entry=catalogs[seat][id]
  if not playable(entry,kind) then return end
  sequence=sequence+1
  local key=hand..'.emote.'..sequence
  if play(player,entry,kind,key) then
    return COOP_ExchangeEmit('emote','emote',tostring(seat),kind..'.'..id,key)==1
  end
end
if not guest then
  COOP_ApproveLocalEmote=function(player,game,kind,id)
    if game==COOP_EmoteTable() and player==COOP_ControlLocalPlayer(game) then
      return approve(player.index,game.handNumber,kind,id)
    end
  end
end

COOP_EmoteReceive=function(dlg,node,value,key)
  local seat=tonumber(node)
  local kind,id=value:match('^([1-3])%.(%d+)$')
  local hand,seq=key:match('^(%d+)%.emote%.(%d+)$')
  if not guest or dlg~='emote' or not seat or seat<1 or seat>4 or seat%1~=0 or not id or not seq then return end
  local game=COOP_EmoteTable()
  if not game or tonumber(hand)~=game.handNumber or tonumber(seq)<=lastSequence then return end
  lastSequence=tonumber(seq)
  local player=game.players[seat]
  if not owns(player) or busy() then return end
  loadCatalog()
  local entry=catalogs[seat][tonumber(id)]
  if playable(entry,tonumber(kind)) then play(player,entry,tonumber(kind),key) end
end

-- Stop an optional emote before the next authored conversation or poker line.
for _,name in ipairs({'StartBanter','PlayPokerLine','PlayLine','PokerAction'}) do
  local original=Dialogue[name]
  if type(original)=='function' then
    Dialogue[name]=function(self,...)
      if #controllers>0 then stop() end
      return original(self,...)
    end
  end
end

local function clip(value,limit)
  if #value<=limit then return value end
  local edge=limit-3
  local byte=value:byte(edge+1)
  while byte and byte>=128 and byte<192 do edge=edge-1;byte=value:byte(edge+1) end
  local prefix=value:sub(1,edge)
  local words=prefix:match('^(.*)%s+%S*$')
  return ((words and #words>=edge/2) and words or prefix)..'...'
end

local function preview(entry)
  if entry.preview~=nil then return entry.preview end
  entry.preview=''
  if not entry.dialog or type(DlgGetExchangeNodeTextIDs)~='function' or
     type(DlgGetExchangeNodeText)~='function' then return entry.preview end
  -- Both engine functions return ordered arrays. Join on the language ID
  -- so multi-speaker exchanges show the owned character's line.
  local node='dlg_id: '..entry.node
  local idsOK,ids=pcall(DlgGetExchangeNodeTextIDs,entry.dialog,node,true)
  if not idsOK or type(ids)~='table' then return entry.preview end
  for i,id in ipairs(ids) do
    if tostring(id)==entry.line then
      local textOK,texts=pcall(DlgGetExchangeNodeText,entry.dialog,node,true)
      if textOK and type(texts)=='table' and #texts==#ids and type(texts[i])=='string' then
        local line=texts[i]:gsub('%b<>',''):gsub('%b[]',''):gsub('%b{}',''):gsub('%s+',' '):match('^%s*(.-)%s*$')
        entry.preview=clip(line,120)
      end
      break
    end
  end
  return entry.preview
end

local function inFilter(entry)
  local group=groups[page-1]
  return playable(entry,mode) and (not group or entry.name:find(group,1,true))
end

local function matchesQuery(entry)
  local text=entry.search..' '..(entry.preview or ''):lower()
  for word in query:gmatch('%S+') do
    if not text:find(word,1,true) then return false end
  end
  return true
end

local function filter(preserve)
  local current=preserve and rows[selection]
  rows={}
  for _,entry in ipairs(catalogs[localPlayer.index]) do
    if inFilter(entry) and matchesQuery(entry) then rows[#rows+1]=entry end
  end
  if alphabetical then
    table.sort(rows,function(a,b) if a.title==b.title then return a.id<b.id end;return a.title<b.title end)
  end
  selection=1
  if current then
    for i,entry in ipairs(rows) do if entry==current then selection=i;break end end
  end
end

local function refresh(preserve)
  labelCursor=query:find('%S') and 1 or nil
  filter(preserve)
end

local function scanLabels()
  if not labelCursor then return false end
  local entries=catalogs[localPlayer.index]
  local changed=false
  -- Index dialogue only during search, with bounded work per frame.
  for i=labelCursor,math.min(labelCursor+15,#entries) do
    local entry=entries[i]
    if entry.preview==nil and inFilter(entry) and not matchesQuery(entry) then
      if preview(entry)~='' and matchesQuery(entry) then changed=true end
    end
  end
  labelCursor=labelCursor+16
  if labelCursor>#entries then labelCursor=nil end
  if changed then filter(true) end
  return changed or not labelCursor
end

local white={r=0.88,g=0.91,b=0.96,a=1}
local pale={r=0.55,g=0.70,b=0.91,a=1}
local dark={r=0.025,g=0.035,b=0.055,a=0.96}
local selected={r=0.14,g=0.28,b=0.49,a=0.97}
local control={r=0.08,g=0.14,b=0.24,a=0.95}
local muted={r=0.56,g=0.61,b=0.68,a=1}
local panel={x=0.075,y=0.075,w=0.85,h=0.89}
local function text(name,value,x,y,scale,color,background)
  local id=agents[name]
  if not id or not AgentExists(id) then
    id='coop_emote_'..name
    AgentCreate(id,'module_text.prop',Vector(0,0,0),nil,kUIScene,false,false)
    agents[name]=id
    AgentSetProperty(id,'Text Font','Futura_LT_Book_24.font')
    AgentSetProperty(id,'Text Alignment Horizontal','Left Justified')
    AgentSetProperty(id,'Text Alignment Vertical','Top')
    AgentSetProperty(id,'Text Render Layer',name=='panel' and 28000 or 29000)
  end
  AgentSetPos(id,Vector(x,y,0))
  AgentSetProperty(id,'Text String',value)
  AgentSetProperty(id,'Text Scale',scale)
  AgentSetProperty(id,'Text Color',color)
  AgentSetProperty(id,'Text Background',background~=nil)
  if background then AgentSetProperty(id,'Text Background Color',background) end
  AgentHide(id,false)
end

local function draw()
  hitTargets={}
  local camera='cam_ui_player'
  if not AgentExists(camera) then return end
  local pos=AgentGetPos(camera)
  local screen=RenderGetScreenResolution()
  local fov=AgentGetProperty(camera,'Field of View')
  local fovScale=AgentGetProperty(camera,'Field of View Scale')
  if not pos or not screen or screen.x<=0 or screen.y<=0 or type(fov)~='number' or pos.z<=0 then return end
  local h=pos.z*math.tan(fov*(type(fovScale)=='number' and fovScale or 1)*math.pi/480)
  local aspect=AgentGetProperty(camera,'Aspect Ratio')
  local w=h*((type(aspect)=='number' and aspect>0) and aspect or screen.x/screen.y)
  local scale=math.min(1,w/9,h/5.6)
  local compact=w/h<1.55
  local function at(name,value,x,y,size,color,background)
    text(name,value,pos.x+(x*2-1)*w,pos.y+(1-y*2)*h,size*scale,color,background)
  end
  local function button(name,value,x,y,width,height,size,kind,id,active,enabled)
    at(name,value,x,y,size,enabled and white or muted,active and selected or control)
    AgentSetProperty(agents[name],'Text Min Width',2*w*width)
    AgentSetProperty(agents[name],'Text Min Height',2*h*height)
    if enabled then hitTargets[#hitTargets+1]={x=x,y=y,w=width,h=height,kind=kind,id=id} end
  end
  at('panel',' ',panel.x,panel.y,1,white,dark)
  AgentSetProperty(agents.panel,'Text Min Width',2*w*panel.w)
  AgentSetProperty(agents.panel,'Text Min Height',2*h*panel.h)
  at('title',localPlayer:GetName():upper()..'  /  EMOTES',0.10,0.10,0.68,white)
  button('close',' Close ',0.81,0.10,0.09,0.047,0.37,'close',nil,false,true)
  for i,category in ipairs(categories) do
    button('tab'..i,' '..category..' ',0.10+(i-1)*0.162,0.172,0.152,0.052,0.37,'tab',i,page==i,true)
  end
  button('type',' '..modes[mode]..' ',0.10,0.241,0.232,0.051,0.37,'key',10,false,true)
  button('sort',alphabetical and ' Sort: A-Z ' or ' Asset order ',0.346,0.241,0.148,0.051,0.37,'key',11,false,true)
  local searchText=query=='' and (searching and '_' or 'Search...') or (clip(query,compact and 20 or 29)..(searching and '_' or ''))
  button('search',' '..searchText..' ',0.510,0.241,0.315,0.051,0.37,'key',14,searching,true)
  button('clear',' Clear ',0.837,0.241,0.063,0.051,0.32,'clear',nil,false,query~='')

  local top=math.floor((math.max(1,selection)-1)/lines)*lines+1
  for i=1,lines do
    local index=top+i-1
    local entry=rows[index]
    local sample=entry and mode~=1 and preview(entry) or ''
    local label=entry and ((index==selection and '>  ' or '    ')..entry.title..(sample~='' and '  -  '..sample or '')) or ' '
    local y=0.318+(i-1)*0.044
    if entry then
      button('row'..i,clip(label,compact and 53 or 74),0.10,y,0.80,0.041,0.38,'row',index,index==selection,true)
    elseif agents['row'..i] then
      AgentHide(agents['row'..i],true)
    end
  end
  local entry=rows[selection]
  local sample=entry and mode~=1 and preview(entry) or ''
  local description=sample~='' and ('"'..clip(sample,compact and 59 or 85)..'"') or
    (entry and (mode==1 and 'Animation only' or 'Dialogue preview unavailable') or
      (labelCursor and 'Searching voice lines...' or 'No matches'))
  at('detail',description,0.10,0.738,0.40,pale)
  local blocked,inProgress=busy(),playing()
  local status=(blocked and 'Table dialogue active') or (inProgress and 'Emote playing') or
    (labelCursor and 'Searching voice lines...') or (entry and selection..' / '..#rows or '')
  at('status',status,0.10,0.807,0.37,pale)
  button('prev',' <  Prev ',0.51,0.796,0.105,0.056,0.36,'key',7,false,top>1)
  button('next',' Next  > ',0.63,0.796,0.105,0.056,0.36,'key',8,false,top+lines<=#rows)
  button('play',' Play ',0.75,0.796,0.15,0.056,0.42,'key',9,true,entry~=nil and not blocked and not inProgress)
  local help=searching and 'Enter: finish search  /  Esc: exit search' or
    'Arrows: navigate   PgUp/PgDn: page   Tab: type   /: search   Enter: play'
  at('help',help,0.10,0.897,0.32,white)
end

if guest or (COOP_LobbyStatus and select(15,COOP_LobbyStatus())==1) then
  local dirty=true
  local previousBusy,previousPlaying,previousW,previousH
  local function changePage(nextPage)
    pageModes[page]=mode
    page=nextPage
    mode=pageModes[page] or 3
    searching=false
    refresh(false)
  end
  local function operate(key,game)
    if key==3 or key==4 then
      changePage((page-1+(key==3 and -1 or 1))%#categories+1)
    elseif key==10 then mode=mode%#modes+1;refresh(true)
    elseif key==11 then alphabetical=not alphabetical;filter(true)
    elseif key==14 then searching=true
    elseif key==9 and not (busy() or playing()) and rows[selection] then
      if COOP_RequestCastEmote(game,mode,rows[selection].id) then close() end
    elseif key==5 then selection=math.max(1,selection-1)
    elseif key==6 then selection=math.min(math.max(1,#rows),selection+1)
    elseif key==7 then selection=math.max(1,selection-lines)
    elseif key==8 then selection=math.min(math.max(1,#rows),selection+lines)
    elseif key==12 then selection=1
    elseif key==13 then selection=math.max(1,#rows)
    end
    dirty=true
  end

  -- Use PN2's cursor conversion and mouse events, as the lobby does. Handle
  -- clicks on the game thread and keep clicks outside the overlay available
  -- to the normal poker controls.
  local press,release=ButtonManager_OnMouseLPress,ButtonManager_OnMouseLRelease
  if press and release and CursorGetPos and DeviceToViewport then
    local function contains(rect,pos)
      return pos.x>=rect.x and pos.x<=rect.x+rect.w and pos.y>=rect.y and pos.y<=rect.y+rect.h
    end
    ButtonManager_OnMouseLPress=function(event,...)
      consumeRelease=false
      if open and select(5,COOP_LobbyInput('focus'))==1 then
        local device=CursorGetPos()
        local pos=device and DeviceToViewport(device)
        if pos and contains(panel,pos) then
          consumeRelease=true
          for _,target in ipairs(hitTargets) do
            if contains(target,pos) then pendingClick=target;break end
          end
          return
        end
      end
      return press(event,...)
    end
    ButtonManager_OnMouseLRelease=function(event,...)
      if consumeRelease then consumeRelease=false;return end
      return release(event,...)
    end
  end

  ThreadStart(function()
    while true do
      local key,focused=COOP_EmoteInput(searching and 'search' or '')
      local game=COOP_EmoteTable()
      local player=game and COOP_ControlLocalPlayer(game)
      if not owns(player) then player=nil end
      if open and (focused~=1 or player~=localPlayer or not gameplay() or InGameMenu_IsMenuActive()) then close() end
      if key==1 then
        if open then close()
        elseif player and gameplay() and AgentExists('cam_ui_player') and not InGameMenu_IsMenuActive() then
          localPlayer=player;loadCatalog();open=true;refresh(false);dirty=true
        end
      elseif open and pendingClick then
        local target=pendingClick
        pendingClick=nil
        if target.kind=='tab' then changePage(target.id)
        elseif target.kind=='row' then selection=target.id;searching=false
        elseif target.kind=='close' then close()
        elseif target.kind=='clear' then query='';refresh(false)
        else
          if target.id~=14 then searching=false end
          operate(target.id,game)
        end
        dirty=true
      elseif open and key==2 then
        if searching then searching=false;dirty=true else close() end
      elseif open and searching then
        if key==9 then searching=false;dirty=true
        elseif key==108 then query=query:sub(1,-2);refresh(false);dirty=true
        elseif key and key>=132 and #query<30 then
          query=query..string.char(key-100):lower();refresh(false);dirty=true
        end
      elseif open and key and key>2 then
        operate(key,game)
      end
      if open then
        local labelsChanged=scanLabels()
        local isBusy=busy()
        local isPlaying=playing()
        local w,h=COOP_LobbyInput('size')
        if dirty or labelsChanged or isBusy~=previousBusy or isPlaying~=previousPlaying or w~=previousW or h~=previousH then
          draw()
          dirty=false;previousBusy,previousPlaying,previousW,previousH=isBusy,isPlaying,w,h
        end
      end
      if #controllers>0 and busy() then stop() end
      Yield()
    end
  end)
end
if not guest then
  ThreadStart(function()
    while true do
      local seat,hand,kind,id=COOP_EmoteTake()
      if seat then approve(seat,hand,kind,id) end
      if #controllers>0 and busy() then stop() end
      Yield()
    end
  end)
end
COOP_EmotesInstalled=true
