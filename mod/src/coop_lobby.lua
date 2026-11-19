-- Native menu UI installs before role-dependent gameplay wrappers.
if COOP_LobbyInstalled then return end
assert(type(InGameMenu_IsMenuActive)=='function' and type(InGameMenu_OnStartNewTournament)=='function' and type(AgentCreate)=='function','lobby menu not loaded')
assert(type(COOP_LobbyInput)=='function','lobby input unavailable')
assert(type(COOP_SessionBegin)=='function','clean session unavailable')
for _,name in ipairs({'Credits_AreRunning','InGameMenu_InMenuFromBoot','InGameMenu_GetCurrentPage','InGameMenu_IsStartingGame',
  'ButtonManager_EnableInput','ButtonManager_DisableInput','ButtonManager_OnMouseLPress','ButtonManager_OnMouseLRelease',
  'CursorGetPos','DeviceToViewport','MenuManager_EnableInput','DialogBox_PickOmaha','ThreadStart','SceneFind','AgentExists','AgentGetPos','AgentSetPos','RenderGetScreenResolution','GetPreferences'}) do
  assert(type(_G[name])=='function','lobby dependency unavailable: '..name)
end
local page,enabled,starting='closed',false,false
local fields,buttons={},{}
local pool,scene={},nil
local layout=nil
local nativeDialog,pendingClick,consumeRelease=false,nil,false
local selected,count,address=1,2,'127.0.0.1'
local notice=''
local function trace(message) if type(Print)=='function' then Print('COOP_LOBBY '..message) end end
local names={'Brock','Claptrap','Ash','Sam'}
local function canOpen()
  return InGameMenu_IsMenuActive() and InGameMenu_InMenuFromBoot() and not InGameMenu_IsStartingGame() and
    (not kMenuPage_MainMenu or InGameMenu_GetCurrentPage()==kMenuPage_MainMenu) and not Credits_AreRunning()
end
local function status()
  local mode,phase,guest,peer,id,rev,guests,connected,ready,s2,s3,s4,s5,err=COOP_LobbyStatus()
  return {mode=mode,phase=phase,guest=guest==1,peer=peer,id=id,rev=rev,count=guests,connected=connected,ready=ready,seats={s2,s3,s4,s5},err=err}
end
local function bit(mask,index) return math.floor(mask/2^index)%2==1 end
local function menuLayout()
  if not AgentExists('cam_menuMain_overlay') or not AgentExists('txt_menumain_overlay_header') then return nil end
  -- The shipped overlay camera faces -Z from (0,0,30); its authored text is
  -- on Z=25. Project onto that plane explicitly. The screen-position helper
  -- dereferences a newly created agent's unassigned camera and can crash C++.
  local camera=AgentGetPos('cam_menuMain_overlay')
  local header=AgentGetPos('txt_menumain_overlay_header')
  local resolution=RenderGetScreenResolution()
  local fov=AgentGetProperty('cam_menuMain_overlay','Field of View')
  local scale=AgentGetProperty('cam_menuMain_overlay','Field of View Scale')
  local aspect=AgentGetProperty('cam_menuMain_overlay','Aspect Ratio')
  if not camera or not header or not resolution or resolution.x<=0 or resolution.y<=0 or
    type(fov)~='number' or type(scale)~='number' or fov*scale<=0 or fov*scale>=179 or camera.z<=header.z then return nil end
  -- PN2's projection uses a 0.75 degree conversion before the half-angle
  -- (native constant 0.013089969754 at 0x94d554), rather than raw vertical FOV.
  local halfHeight=(camera.z-header.z)*math.tan(fov*scale*math.pi/480)
  return {x=camera.x,y=camera.y,z=header.z,h=halfHeight,w=halfHeight*((type(aspect)=='number' and aspect>0) and aspect or resolution.x/resolution.y)}
end
local function set(name,text,x,y,button)
  local agent=pool[name]
  if not agent or not AgentExists(agent) then
    fields[name]=nil
    agent='coop_lobby_'..name
    AgentCreate(agent,'module_text.prop',Vector(0,0,0),nil,scene,false,false)
    pool[name]=agent
    local font=AgentExists('txt_menumain_overlay_header') and AgentGetProperty('txt_menumain_overlay_header','Text Font')
    AgentSetProperty(agent,'Text Font',font or 'Futura_LT_Book_24.font')
    AgentSetProperty(agent,'Text Alignment Horizontal','Left Justified')
    AgentSetProperty(agent,'Text Alignment Vertical','Top')
    AgentSetProperty(agent,'Text Render Layer',29000)
    AgentSetProperty(agent,'Text Color',{r=1,g=1,b=1,a=1})
    AgentSetProperty(agent,'Text Scale',name=='title' and 0.9 or 0.65)
  end
  if fields[name]~=text then AgentSetProperty(agent,'Text String',text);fields[name]=text end
  AgentSetPos(agent,Vector(layout.x+(2*x-1)*layout.w,layout.y+(1-2*y)*layout.h,layout.z))
  AgentHide(agent,false)
  if button then buttons[#buttons+1]={name=name,x=x,y=y,w=0.28,h=0.055,run=button} end
end
local function hide()
  for _,agent in pairs(pool) do if AgentExists(agent) then AgentHide(agent,true) end end
  buttons={}
end
local function close()
  hide();page='closed';ButtonManager_EnableInput();MenuManager_EnableInput(true)
end
local function action(verb,arg)
  if COOP_LobbyAction(verb,arg or '')~=1 then trace('action='..verb..' result=rejected');notice='Unable to do that yet. Please try again.';return false end
  trace('action='..verb..' result=accepted')
  notice='';return true
end
local function open()
  if page~='closed' or not canOpen() then return end
  page='home';notice='';ButtonManager_DisableInput();MenuManager_EnableInput(false);CursorHide(false)
end
local function hit(x,y)
  for _,button in ipairs(buttons) do
    if x>=button.x and x<=button.x+button.w and y>=button.y and y<=button.y+button.h then return button end
  end
end
-- Use the same engine events and device-to-viewport cursor conversion as the
-- shipped UI. Snapshot a named target on press; run it only on the menu thread,
-- where save dialogs and match startup may safely Yield. Windows frame polling
-- can miss a complete click, and OS cursor coordinates need not be engine ones.
local press,release=ButtonManager_OnMouseLPress,ButtonManager_OnMouseLRelease
local clickLogs=0
ButtonManager_OnMouseLPress=function(event,...)
  if layout and not starting and not nativeDialog and (page~='closed' or canOpen()) then
    local _,_,_,_,focused=COOP_LobbyInput('focus')
    if focused==1 then
      local position=DeviceToViewport(CursorGetPos())
      local button=hit(position.x,position.y)
      if clickLogs<32 then
        trace('mouse_press page='..page..' x='..tostring(position.x)..' y='..tostring(position.y)..' hit='..(button and button.name or 'none'))
        clickLogs=clickLogs+1
      end
      if button or page~='closed' then
        if button and not pendingClick then pendingClick={page=page,name=button.name} end
        consumeRelease=true
        return
      end
    end
  end
  return press(event,...)
end
ButtonManager_OnMouseLRelease=function(event,...)
  if consumeRelease then consumeRelease=false;return end
  if layout and page~='closed' and not starting and not nativeDialog then return end
  return release(event,...)
end
local function beginMatch()
  if starting then return end
  starting=true;hide();ButtonManager_EnableInput();MenuManager_EnableInput(true)
  while COOP_LobbyInstall()~='ok' do
    if status().phase~=3 then starting=false;page='lobby';notice='Connection ended before match startup.';return end
    Yield()
  end
  COOP_SessionBegin()
  -- Use the actual shipped front-end start path, including its transitions,
  -- menu transition. The clean session suppresses campaign reload and the
  -- cinematic through the original initialized skip-opening branch.
  AgentSetProperty('logic_persistent','sGameType',kMode_TexasHoldem)
  local approve,choose,initialize=GameObject.OnClobberSaveGame,DialogBox_PickOmaha,GameTable.Initialize
  local pendingApproval,pendingMode=true,true
  GameObject.OnClobberSaveGame=function(self,...)
    if pendingApproval then pendingApproval=false;return true end
    return approve(self,...)
  end
  DialogBox_PickOmaha=function(...)
    if pendingMode then pendingMode=false;return kResult_Yes end -- authored short start; session supplies clean table
    return choose(...)
  end
  GameTable.Initialize=function(self,...)
    pendingApproval=false;pendingMode=false
    GameObject.OnClobberSaveGame=approve;DialogBox_PickOmaha=choose;GameTable.Initialize=initialize
    return initialize(self,...)
  end
  InGameMenu_OnStartNewTournament()
end
local function ready(s)
  local index=s.guest and s.peer-1 or 0
  -- Multiplayer is temporary and cannot replace the solo save. A clobber-save
  -- confirmation here would be misleading and depend on each client's history.
  action('ready',bit(s.ready,index) and '0' or '1')
end
local function resultsVote(choice)
  if COOP_MatchAction('vote',tostring(choice))~=1 then
    notice='Could not send your choice.'
  else
    notice=''
  end
end
local function drawResults(s)
  local rematch,lobby,p1,p2,p3,p4,p5=COOP_MatchStatus()
  local place={p1,p2,p3,p4,p5}
  local human={[5]=true}
  local rematches,returns=0,0
  for i=0,s.count do
    if i>0 and s.seats[i]>0 then human[s.seats[i]]=true end
    if bit(rematch,i) then rematches=rematches+1 end
    if bit(lobby,i) then returns=returns+1 end
  end
  local row=0
  for rank=1,5 do
    for seat=1,5 do
      if place[seat]==rank then
        row=row+1
        local name=names[seat] or 'The Player'
        set('result'..row,tostring(rank)..'.  '..name..(human[seat] and '  /  Human' or '  /  AI'),0.25,0.28+(row-1)*0.069)
      end
    end
  end
  local total=s.count+1
  local index=s.guest and s.peer-1 or 0
  local selectedRematch=bit(rematch,index)
  local selectedLobby=bit(lobby,index)
  local yourChoice=selectedRematch and 'Rematch' or selectedLobby and 'Return to lobby'
  set('rematch','Rematch ('..rematches..'/'..total..')',0.25,0.69,function() resultsVote(selectedRematch and 0 or 1) end)
  set('return','Return to lobby ('..returns..'/'..total..')',0.57,0.69,function() resultsVote(selectedLobby and 0 or 2) end)
  if yourChoice then set('resultWait','Your vote: '..yourChoice..'. Waiting for everyone to agree.',0.25,0.79) end
end
local function draw(s)
  hide()
  if page=='closed' then
    if canOpen() then set('entry','MULTIPLAYER',0.77,0.89,open) end
    return
  end
  set('panel',' ',0.16,0.1)
  AgentSetProperty(pool.panel,'Text Background',true)
  AgentSetProperty(pool.panel,'Text Background Color',{r=0.02,g=0.025,b=0.04,a=0.97})
  AgentSetProperty(pool.panel,'Text Min Width',19)
  AgentSetProperty(pool.panel,'Text Min Height',13)
  AgentSetProperty(pool.panel,'Text Render Layer',28000)
  set('title',page=='results' and 'TOURNAMENT RESULTS' or 'MULTIPLAYER',0.22,0.16)
  if page=='results' then
    drawResults(s)
  elseif page=='home' then
    set('host','Host a lobby',0.25,0.34,function() page='host' end)
    set('join','Find a lobby',0.25,0.45,function() COOP_LobbyScan();page='browse' end)
    set('back','Back',0.25,0.73,close)
  elseif page=='host' then
    set('capacity','Hold\'em / standard table / '..count..' guests',0.25,0.32)
    set('less','Fewer guests',0.25,0.43,function() count=math.max(1,count-1) end)
    set('more','More guests',0.57,0.43,function() count=math.min(4,count+1) end)
    set('create','Create lobby',0.25,0.59,function() if action('host',tostring(count)) then page='lobby' end end)
    set('back','Back',0.25,0.73,function() page='home' end)
  elseif page=='browse' then
    local found=0
    for i=1,4 do
      local ip,port,guests,connected=COOP_LobbyFound(tostring(i))
      if ip then
        found=found+1;local target=ip..':'..port
        set('found'..i,target..'  '..connected..'/'..guests..' guests',0.25,0.27+i*0.075,function() if action('join',target) then page='lobby' end end)
      end
    end
    if found==0 then set('empty','Looking for nearby LAN lobbies...',0.25,0.35) end
    set('refresh','Refresh',0.25,0.66,function() COOP_LobbyScan() end)
    set('manual','Enter host IP',0.57,0.66,function() page='ip' end)
    set('back','Back',0.25,0.75,function() page='home' end)
  elseif page=='ip' then
    set('iptitle','Type host IPv4 address (optional :port)',0.25,0.31)
    set('ipvalue',address..'_',0.25,0.41)
    set('connect','Connect',0.25,0.59,function() if action('join',address) then page='lobby' end end)
    set('back','Back',0.25,0.73,function() page='browse' end)
  elseif page=='lobby' then
    if s.phase==3 then beginMatch();return end
    if s.phase==4 then
      local message=s.err==2 and 'Could not open/connect. Check the host address.' or
        s.err==4 and 'A player did not finish joining. Return and try again.' or
        'Connection ended or was rejected. Return and try again.'
      set('error',message,0.25,0.35)
    else
      set('role',s.guest and 'Choose a character, then Ready' or 'Waiting for '..s.count..' guests',0.25,0.26)
      set('hostrow','The Player — Host'..(bit(s.ready,0) and ' — READY' or ''),0.25,0.34)
      for i=1,s.count do
        set('row'..i,(s.seats[i]>0 and names[s.seats[i]] or 'Choosing character')..(bit(s.connected,i-1) and '' or ' — not connected')..(bit(s.ready,i) and ' — READY' or ''),0.25,0.34+i*0.055)
      end
      if s.guest and s.phase==1 then
        set('prev','< Previous',0.25,0.64,function() selected=selected==1 and 4 or selected-1 end)
        set('next','Next >',0.57,0.64,function() selected=selected==4 and 1 or selected+1 end)
        set('claim','Pick '..names[selected],0.25,0.71,function() action('claim',tostring(selected)) end)
      end
      if s.phase==1 and (not s.guest or (s.peer>=2 and s.seats[s.peer-1]>0)) then
        set('ready',bit(s.ready,s.guest and s.peer-1 or 0) and 'Unready' or 'Ready',0.57,0.71,function() ready(s) end)
      elseif s.phase==2 then set('starting','Everyone ready. Starting match...',0.25,0.68) end
    end
    if s.phase~=3 then set('leave','Leave lobby',0.25,0.8,function() if action('leave') then page='home' end end) end
    if s.err==6 then notice='That character is already taken.' end
  end
  if notice~='' then set('notice',notice,0.22,0.87) end
end
ThreadStart(function()
  local previous,visibility,renderError='','',''
  while true do
    local s=status()
    if s.phase==5 and not starting then page='results'
    elseif page=='results' then page='lobby';notice='' end
    local signature=page..':'..s.phase..':'..s.rev..':'..s.ready
    if signature~=previous then trace('page='..page..' phase='..s.phase..' rev='..s.rev..' ready='..s.ready..' guests='..s.count);previous=signature end
    -- A saved table leaves Logic.bInGame true even on the boot main menu.
    -- The front-end's own lifetime distinguishes it from the gameplay pause UI.
    local frontMenu=InGameMenu_IsMenuActive() and InGameMenu_InMenuFromBoot()
    scene=SceneFind('ui_menuMain_overlay.scene') and 'ui_menuMain_overlay.scene'
    layout=scene and frontMenu and not starting and menuLayout() or nil
    local view=tostring(not not scene)..':'..tostring(frontMenu)..':'..tostring(canOpen())..':'..tostring(not not layout)
    if view~=visibility then trace('visibility scene='..tostring(not not scene)..' boot_menu='..tostring(frontMenu)..' entry='..tostring(canOpen())..' layout='..tostring(not not layout)..' saved_game_flag='..tostring(Logic and Logic.bInGame));visibility=view end
    if layout and not starting then
      if page~='closed' then ButtonManager_DisableInput();MenuManager_EnableInput(false) end
      if page=='closed' then
        -- Entry drawing cannot Yield. Capture engine asset/property errors here
        -- without wrapping normal menu actions or match startup across pcall.
        local ok,err=pcall(draw,s)
        if not ok then
          if renderError~=tostring(err) then trace('entry_render_error '..tostring(err));renderError=tostring(err) end
          buttons={}
        elseif renderError~='' then trace('entry_render_recovered');renderError='' end
      else draw(s) end
      local _,_,_,key=COOP_LobbyInput(page=='ip' and 'ip' or '')
      if key and key>0 and page=='ip' then
        if key==8 then address=address:sub(1,-2)
        elseif key==13 then if action('join',address) then page='lobby' end
        elseif #address<21 then address=address..string.char(key) end
      end
      local queued=pendingClick;pendingClick=nil
      if queued and queued.page==page then
        for _,button in ipairs(buttons) do
          if button.name==queued.name then trace('activate page='..page..' button='..button.name);button.run();break end
        end
      end
    elseif not starting then pendingClick=nil;hide() end
    Yield()
  end
end)
COOP_LobbyInstalled=true
trace('installed native_frontend')
