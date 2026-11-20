-- The original settings arrows and controller select this same row.
if COOP_DisplayModeInstalled then return end
if type(UI_IGM_Settings_InitRezSettings)~='function' or
   type(UI_IGM_Settings_ToggleFullScreen)~='function' or
   type(UI_IGM_Settings_Graphics_ApplyChangesAndConfirm)~='function' or
   type(InGameMenu_OnLeft)~='function' then return end

local statusAgent='ui_menuMain_settings_fullScreenStatus'
local leftArrow='ui_menu_table_settings_fullScreen_arrowLeft'
local initialize,toggle,left=UI_IGM_Settings_InitRezSettings,UI_IGM_Settings_ToggleFullScreen,InGameMenu_OnLeft
local busy=false
local function mode()
  if not GetPreferences().Windowed then return 'Fullscreen' end
  return COOP_DisplayWindow('active')==1 and 'Borderless' or 'Windowed'
end
local function label()
  if not AgentExists(statusAgent) then return end
  AgentSetProperty(statusAgent,'Text Dialog 2.0 Node Name','')
  AgentSetProperty(statusAgent,'Text String',mode())
end

UI_IGM_Settings_InitRezSettings=function(...)
  initialize(...)
  label()
end

local function cycle(button, reverse)
  if busy then return end
  busy=true
  local current=mode()
  if current=='Borderless' then
    if COOP_DisplayWindow('leave')==1 then
      if not reverse then toggle(button) end
    end
  elseif current=='Windowed' then
    if reverse then toggle(button)
    else COOP_DisplayWindow('enter') end
  else
    toggle(button)
    if reverse and GetPreferences().Windowed then COOP_DisplayWindow('enter') end
  end
  label()
  busy=false
end

UI_IGM_Settings_ToggleFullScreen=function(button)
  return cycle(button,button and button.agent and AgentGetName(button.agent)==leftArrow)
end

InGameMenu_OnLeft=function(...)
  if InGameMenu_IsMenuActive() and InGameMenu_GetCurrentPage()==kMenuPage_Settings and
     UI_IGM_Settings_GetCurrentSettingsTab()==UI_IGM_Settings_GetSettingsTab_Graphics() then
    local button=ButtonManager_GetSelected()
    if button and AgentGetName(button.agent)==statusAgent then
      if not ThreadIsRunning(IGM_mFullScreenToggleThread) then
        IGM_mFullScreenToggleThread=ThreadStart(cycle,button,true)
      end
      return
    end
  end
  return left(...)
end

-- The stock resolution confirmation may resize the window during both apply
-- and rollback. Keep the same monitor frame after the dialog closes.
local apply=UI_IGM_Settings_Graphics_ApplyChangesAndConfirm
UI_IGM_Settings_Graphics_ApplyChangesAndConfirm=function(...)
  apply(...)
  if mode()=='Borderless' then COOP_DisplayWindow('refresh') end
end

COOP_DisplayModeInstalled=true
