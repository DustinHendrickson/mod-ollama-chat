-- Ollama Monitor: a live debug window for mod-ollama-chat's autopilot bots.
--
-- Talks to the server by whispering its own character on the addon channel
-- ("OAPM\t<request>"); the server answers the same way. See
-- src/mod-ollama-chat_monitor.cpp for the protocol. Needs a GM account at
-- OllamaChat.Monitor.MinSecurity or above.

local PREFIX = "OAPM"

local PAGES = {
    { key = "overview", label = "Overview", every = 1.5 },
    { key = "travel",   label = "Travel",   every = 1.5 },
    { key = "planner",  label = "Planner",  every = 10 },
    { key = "chat",     label = "Chat",     every = 3 },
    { key = "mind",     label = "Mind",     every = 5 },
    { key = "events",   label = "Events",   every = 3 },
}

local CLASS_FILES = {
    [1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE", [5] = "PRIEST",
    [6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID",
}

local STATE_COLORS = {
    ["fighting"]             = "ff4040",
    ["dead"]                 = "a0a0a0",
    ["corpse run"]           = "a0a0a0",
    ["travelling"]           = "40c0ff",
    ["flying"]               = "40c0ff",
    ["aboard"]               = "40c0ff",
    ["waiting on the model"] = "ffd040",
    ["idle"]                 = "ff9040",
}

local LIST_EVERY = 5
local ROWS       = 20
local ROW_HEIGHT = 18

local M = {
    bots      = {},      -- sorted list of { guid, name, level, class, zone, tier, state, doing }
    incoming  = nil,     -- list being received
    selected  = nil,     -- guid (string)
    page      = "overview",
    pending   = nil,     -- page being received: { guid, page, lines, partial }
    waiting   = false,   -- a page request is out
    askedAt   = 0,
    pageAt    = 0,
    listAt    = 0,
    plain     = false,
    watching  = nil,     -- guid the camera follows
    watchNote = "",
    allowed   = nil,     -- nil until the server answers HELLO
    filter    = "",
}

local function Send(msg)
    SendAddonMessage(PREFIX, msg, "WHISPER", UnitName("player"))
end

local function Print(msg)
    DEFAULT_CHAT_FRAME:AddMessage("|cff60c0ff[Ollama Monitor]|r " .. msg)
end

local function Split(text)
    local out, from = {}, 1
    while true do
        local at = string.find(text, "\t", from, true)
        if not at then
            table.insert(out, string.sub(text, from))
            return out
        end
        table.insert(out, string.sub(text, from, at - 1))
        from = at + 1
    end
end

local function BotByGuid(guid)
    for _, b in ipairs(M.bots) do
        if b.guid == guid then return b end
    end
    return nil
end

local function ClassColor(class)
    local c = RAID_CLASS_COLORS[CLASS_FILES[class] or ""]
    if not c then return "ffffff" end
    return string.format("%02x%02x%02x", c.r * 255, c.g * 255, c.b * 255)
end

-- ---------------------------------------------------------------------------
-- Frame
-- ---------------------------------------------------------------------------

local frame = CreateFrame("Frame", "OllamaMonitorFrame", UIParent)
frame:SetSize(900, 540)
frame:SetPoint("CENTER")
frame:SetFrameStrata("HIGH")
frame:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
frame:SetBackdropColor(0, 0, 0, 0.92)
frame:EnableMouse(true)
frame:SetMovable(true)
frame:SetClampedToScreen(true)
frame:RegisterForDrag("LeftButton")
frame:SetScript("OnDragStart", frame.StartMoving)
frame:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    local point, _, relPoint, x, y = self:GetPoint()
    OllamaMonitorDB = OllamaMonitorDB or {}
    OllamaMonitorDB.point = { point, relPoint, x, y }
end)
frame:Hide()
tinsert(UISpecialFrames, "OllamaMonitorFrame")   -- Escape closes it

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", 0, -18)
title:SetText("Ollama Monitor")

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -6, -6)

local status = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
status:SetPoint("TOPLEFT", 22, -22)
status:SetJustifyH("LEFT")

-- ---------------------------------------------------------------------------
-- Bot list (left)
-- ---------------------------------------------------------------------------

local listPanel = CreateFrame("Frame", nil, frame)
listPanel:SetPoint("TOPLEFT", 18, -46)
listPanel:SetSize(270, 476)
listPanel:SetBackdrop({
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
})
listPanel:SetBackdropColor(0.05, 0.05, 0.08, 0.9)

local filterBox = CreateFrame("EditBox", "OllamaMonitorFilter", listPanel, "InputBoxTemplate")
filterBox:SetSize(160, 20)
filterBox:SetPoint("TOPLEFT", 12, -6)
filterBox:SetAutoFocus(false)
filterBox:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
filterBox:SetScript("OnEnterPressed", function(self) self:ClearFocus() end)

local countText = listPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
countText:SetPoint("LEFT", filterBox, "RIGHT", 8, 0)

local listScroll = CreateFrame("ScrollFrame", "OllamaMonitorListScroll", listPanel, "FauxScrollFrameTemplate")
listScroll:SetPoint("TOPLEFT", 6, -30)
listScroll:SetPoint("BOTTOMRIGHT", -28, 6)

local rows = {}
local RefreshList

local function Visible()
    if M.filter == "" then return M.bots end
    local out, needle = {}, string.lower(M.filter)
    for _, b in ipairs(M.bots) do
        local hay = string.lower(b.name .. " " .. b.zone .. " " .. b.state .. " " .. b.doing)
        if string.find(hay, needle, 1, true) then table.insert(out, b) end
    end
    return out
end

local SelectBot

for i = 1, ROWS do
    local row = CreateFrame("Button", nil, listPanel)
    row:SetSize(232, ROW_HEIGHT)
    row:SetPoint("TOPLEFT", listScroll, "TOPLEFT", 2, -(i - 1) * ROW_HEIGHT)
    row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD")

    row.sel = row:CreateTexture(nil, "BACKGROUND")
    row.sel:SetAllPoints()
    row.sel:SetTexture(0.2, 0.5, 1, 0.25)
    row.sel:Hide()

    row.name = row:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    row.name:SetPoint("LEFT", 4, 0)
    row.name:SetWidth(120)
    row.name:SetJustifyH("LEFT")

    row.state = row:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    row.state:SetPoint("RIGHT", -4, 0)
    row.state:SetWidth(108)
    row.state:SetJustifyH("RIGHT")

    row:SetScript("OnClick", function(self)
        if self.guid then SelectBot(self.guid) end
    end)
    row:SetScript("OnEnter", function(self)
        local b = self.guid and BotByGuid(self.guid)
        if not b then return end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine(b.name .. " (" .. b.level .. ")", 1, 1, 1)
        GameTooltip:AddLine(b.zone, 0.8, 0.8, 0.8)
        GameTooltip:AddLine("tier: " .. b.tier .. " | " .. b.state, 0.6, 0.8, 1)
        if b.doing ~= "" then GameTooltip:AddLine(b.doing, 1, 0.82, 0, true) end
        GameTooltip:Show()
    end)
    row:SetScript("OnLeave", function() GameTooltip:Hide() end)
    rows[i] = row
end

RefreshList = function()
    local list = Visible()
    FauxScrollFrame_Update(listScroll, #list, ROWS, ROW_HEIGHT)
    local offset = FauxScrollFrame_GetOffset(listScroll)
    for i = 1, ROWS do
        local row, b = rows[i], list[offset + i]
        if b then
            row.guid = b.guid
            row.name:SetText(string.format("|cff%s%s|r |cff909090%d|r", ClassColor(b.class), b.name, b.level))
            row.state:SetText(string.format("|cff%s%s|r", STATE_COLORS[b.state] or "c0c0c0", b.state))
            if b.guid == M.selected then row.sel:Show() else row.sel:Hide() end
            row:Show()
        else
            row.guid = nil
            row:Hide()
        end
    end
    countText:SetText(#list .. " of " .. #M.bots .. " bots")
end

listScroll:SetScript("OnVerticalScroll", function(self, offset)
    FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, RefreshList)
end)
filterBox:SetScript("OnTextChanged", function(self)
    M.filter = self:GetText() or ""
    RefreshList()
end)

-- ---------------------------------------------------------------------------
-- Detail (right)
-- ---------------------------------------------------------------------------

local detail = CreateFrame("Frame", nil, frame)
detail:SetPoint("TOPLEFT", listPanel, "TOPRIGHT", 8, 0)
detail:SetPoint("BOTTOMRIGHT", -18, 18)

local header = detail:CreateFontString(nil, "OVERLAY", "GameFontNormal")
header:SetPoint("TOPLEFT", 4, -2)
header:SetPoint("RIGHT", -4, 0)
header:SetJustifyH("LEFT")
header:SetText("Select a bot on the left.")

local function MakeButton(label, width, onClick)
    local b = CreateFrame("Button", nil, detail, "UIPanelButtonTemplate")
    b:SetSize(width, 20)
    b:SetText(label)
    b:SetScript("OnClick", onClick)
    return b
end

local function SelectedBot()
    return M.selected and BotByGuid(M.selected)
end

local followButton = MakeButton("Follow camera", 110, function()
    if M.watching then
        Send("UNWATCH")
    elseif M.selected then
        Send("WATCH " .. M.selected)
    end
end)
followButton:SetPoint("TOPLEFT", 0, -20)

local gotoButton = MakeButton("Teleport to", 90, function()
    if M.selected then Send("GOTO " .. M.selected) end
end)
gotoButton:SetPoint("LEFT", followButton, "RIGHT", 4, 0)

local replanButton = MakeButton("Replan now", 90, function()
    local b = SelectedBot()
    if b then SendChatMessage(".ollama autopilot replan " .. b.name, "SAY") end
end)
replanButton:SetPoint("LEFT", gotoButton, "RIGHT", 4, 0)

local statusButton = MakeButton("Status to chat", 100, function()
    local b = SelectedBot()
    if b then SendChatMessage(".ollama autopilot status " .. b.name, "SAY") end
end)
statusButton:SetPoint("LEFT", replanButton, "RIGHT", 4, 0)

local plainButton = MakeButton("Plain text", 80, function(self)
    M.plain = not M.plain
    self:SetText(M.plain and "Colours" or "Plain text")
    M.Render(true)
end)
plainButton:SetPoint("LEFT", statusButton, "RIGHT", 4, 0)

local tabs = {}
local ShowPage
for i, p in ipairs(PAGES) do
    local t = MakeButton(p.label, 80, function() ShowPage(p.key) end)
    if i == 1 then
        t:SetPoint("TOPLEFT", followButton, "BOTTOMLEFT", 0, -6)
    else
        t:SetPoint("LEFT", tabs[i - 1], "RIGHT", 2, 0)
    end
    t.key = p.key
    tabs[i] = t
end

local updatedText = detail:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
updatedText:SetPoint("LEFT", tabs[#tabs], "RIGHT", 8, 0)

local textBg = CreateFrame("Frame", nil, detail)
textBg:SetPoint("TOPLEFT", tabs[1], "BOTTOMLEFT", 0, -6)
textBg:SetPoint("BOTTOMRIGHT", 0, 52)
textBg:SetBackdrop({
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
})
textBg:SetBackdropColor(0.02, 0.02, 0.04, 0.95)

local scroll = CreateFrame("ScrollFrame", "OllamaMonitorDetailScroll", textBg, "UIPanelScrollFrameTemplate")
scroll:SetPoint("TOPLEFT", 8, -8)
scroll:SetPoint("BOTTOMRIGHT", -28, 8)

local text = CreateFrame("EditBox", nil, scroll)
text:SetMultiLine(true)
text:SetAutoFocus(false)
text:SetMaxLetters(0)
text:SetFontObject(ChatFontNormal)
text:SetWidth(540)
text:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
-- Read only, but selectable so lines can be copied out (Ctrl+C).
text:SetScript("OnTextChanged", function(self, userInput)
    if userInput and self.shown then self:SetText(self.shown) end
end)
scroll:SetScrollChild(text)
scroll:SetScript("OnSizeChanged", function(self, width) text:SetWidth(math.max(100, width - 4)) end)

-- Camera line and whisper box (bottom)
local cameraText = detail:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
cameraText:SetPoint("BOTTOMLEFT", 2, 32)
cameraText:SetPoint("RIGHT", -2, 0)
cameraText:SetJustifyH("LEFT")

local sayLabel = detail:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
sayLabel:SetPoint("BOTTOMLEFT", 2, 8)
sayLabel:SetText("Whisper:")

local sayBox = CreateFrame("EditBox", "OllamaMonitorSay", detail, "InputBoxTemplate")
sayBox:SetHeight(20)
sayBox:SetPoint("LEFT", sayLabel, "RIGHT", 10, 0)
sayBox:SetPoint("RIGHT", detail, "RIGHT", -4, 0)
sayBox:SetAutoFocus(false)
sayBox:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
sayBox:SetScript("OnEnterPressed", function(self)
    local b = SelectedBot()
    local msg = self:GetText()
    if b and msg and msg ~= "" then
        SendChatMessage(msg, "WHISPER", nil, b.name)
        self:SetText("")
        M.pageAt = 0   -- the Chat page shows the answer soonest
    end
    self:ClearFocus()
end)

-- ---------------------------------------------------------------------------
-- Rendering
-- ---------------------------------------------------------------------------

local function Colorize(line)
    if M.plain then return line end
    if string.sub(line, 1, 2) == "# " then
        return "|cffffd100" .. string.sub(line, 3) .. "|r"
    end
    local key, rest = string.match(line, "^(%s*[%w%s'%-%.%(%)]-):( .*)$")
    if key and #key <= 40 then
        local lower = string.lower(line)
        local valueColor = ""
        if string.find(lower, "fail", 1, true) or string.find(lower, "stuck", 1, true) or
           string.find(lower, "error", 1, true) or string.find(lower, "dropped", 1, true) then
            valueColor = "|cffff6060"
        end
        return "|cff80c0ff" .. key .. ":|r" .. valueColor .. rest .. (valueColor ~= "" and "|r" or "")
    end
    return line
end

M.lines = {}

function M.Render(keepScroll)
    local out = {}
    for i, line in ipairs(M.lines) do out[i] = Colorize(line) end
    local body = table.concat(out, "\n")
    local saved = scroll:GetVerticalScroll()
    text.shown = body
    text:SetText(body)
    if keepScroll then
        -- The edit box lays the text out on the next frame; scroll after it.
        M.restoreScroll = saved
    else
        scroll:SetVerticalScroll(0)
    end
end

local function UpdateHeader()
    local b = SelectedBot()
    if not b then
        if M.selected then
            header:SetText("|cffa0a0a0That bot is no longer on autopilot or online.|r")
        else
            header:SetText("Select a bot on the left.")
        end
        return
    end
    header:SetText(string.format("|cff%s%s|r  level %d  |cffc0c0c0%s|r  -  |cff%s%s|r  (%s)",
        ClassColor(b.class), b.name, b.level, b.zone, STATE_COLORS[b.state] or "c0c0c0", b.state, b.tier))
end

local function UpdateCamera()
    if M.watching then
        local b = BotByGuid(M.watching)
        followButton:SetText("Stop camera")
        cameraText:SetText("|cff60ff60Camera:|r " .. ((b and b.name) or M.watchName or "?") ..
            (M.watchNote ~= "" and (" - " .. M.watchNote) or "") ..
            "  |cff909090(your own character stays where it is)|r")
    else
        followButton:SetText("Follow camera")
        cameraText:SetText("|cff909090Camera: your own." ..
            (M.watchNote ~= "" and (" " .. M.watchNote) or "") .. "|r")
    end
end

local function UpdateTabs()
    for _, t in ipairs(tabs) do
        if t.key == M.page then t:LockHighlight() else t:UnlockHighlight() end
    end
end

local function RequestPage()
    if not M.selected then return end
    M.waiting = true
    M.askedAt = GetTime()
    Send("PAGE " .. M.selected .. " " .. M.page)
end

ShowPage = function(key)
    M.page = key
    OllamaMonitorDB = OllamaMonitorDB or {}
    OllamaMonitorDB.page = key
    M.lines = { "loading..." }
    M.Render(false)
    UpdateTabs()
    RequestPage()
end

SelectBot = function(guid)
    M.selected = guid
    M.lines = { "loading..." }
    M.Render(false)
    UpdateHeader()
    RefreshList()
    RequestPage()
end

-- ---------------------------------------------------------------------------
-- Messages from the server
-- ---------------------------------------------------------------------------

local function OnMessage(msg)
    local f = Split(msg)
    local kind = f[1]

    if kind == "H" then
        M.allowed = true
        status:SetText("autopilot " .. (f[3] or "?"))
        Send("LIST")
    elseif kind == "LB" then
        M.incoming = {}
    elseif kind == "L" and M.incoming then
        table.insert(M.incoming, {
            guid = f[2], name = f[3] or "?", level = tonumber(f[4]) or 0, class = tonumber(f[5]) or 0,
            zone = f[6] or "", tier = f[7] or "", state = f[8] or "", doing = f[9] or "",
        })
    elseif kind == "LZ" and M.incoming then
        table.sort(M.incoming, function(a, b) return a.name < b.name end)
        M.bots = M.incoming
        M.incoming = nil
        RefreshList()
        UpdateHeader()
        UpdateCamera()
    elseif kind == "B" then
        M.pending = { guid = f[2], page = f[3], lines = {}, partial = "" }
    elseif kind == "P" and M.pending then
        local piece = f[3] or ""
        if f[2] == "c" then
            M.pending.partial = M.pending.partial .. piece
        else
            table.insert(M.pending.lines, M.pending.partial .. piece)
            M.pending.partial = ""
        end
    elseif kind == "Z" and M.pending then
        local p = M.pending
        M.pending = nil
        M.waiting = false
        M.pageAt = GetTime()
        if p.guid == M.selected and p.page == M.page then
            M.lines = p.lines
            M.Render(true)
            updatedText:SetText("updated " .. date("%H:%M:%S"))
        end
    elseif kind == "W" then
        local state = f[2]
        M.watchNote = f[6] or ""
        if state == "off" then
            M.watching = nil
        else
            M.watching = f[3]
            M.watchName = f[4]
        end
        UpdateCamera()
    elseif kind == "M" then
        M.waiting = false
        Print(f[2] or "")
    end
end

-- ---------------------------------------------------------------------------
-- Polling
-- ---------------------------------------------------------------------------

local function PageEvery()
    for _, p in ipairs(PAGES) do
        if p.key == M.page then return p.every end
    end
    return 3
end

frame:SetScript("OnUpdate", function(self, elapsed)
    if M.restoreScroll then
        local range = scroll:GetVerticalScrollRange()
        scroll:SetVerticalScroll(math.min(M.restoreScroll, range))
        M.restoreScroll = nil
    end
    if not M.allowed then return end

    local now = GetTime()
    if now - M.listAt >= LIST_EVERY then
        M.listAt = now
        Send("LIST")
    end
    if M.selected and OllamaMonitorDB and OllamaMonitorDB.auto ~= false then
        -- One page request at a time; a lost answer is retried after 10s.
        if (not M.waiting and now - M.pageAt >= PageEvery()) or (M.waiting and now - M.askedAt >= 10) then
            RequestPage()
        end
    end
end)

frame:SetScript("OnShow", function()
    if not M.allowed then Send("HELLO") end
    M.listAt = 0
    UpdateTabs()
    UpdateCamera()
end)

-- ---------------------------------------------------------------------------
-- Compact mode: only the bot's name and the log, small and see-through, so
-- the screen stays free while the camera follows a bot.
-- ---------------------------------------------------------------------------

local FULL_W, FULL_H = 900, 540
local FULL_BACKDROP = {
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
}
local COMPACT_BACKDROP = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
}
-- Everything that is not the log.
local FULL_ONLY = {
    title, status, listPanel, followButton, gotoButton, replanButton, statusButton, plainButton,
    updatedText, cameraText, sayLabel, sayBox,
}
for _, t in ipairs(tabs) do table.insert(FULL_ONLY, t) end

frame:SetResizable(true)
frame:SetMinResize(280, 140)
frame:SetMaxResize(1600, 1200)

local grip = CreateFrame("Button", nil, frame)
grip:SetSize(16, 16)
grip:SetPoint("BOTTOMRIGHT", -4, 4)
grip:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")
grip:SetHighlightTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Highlight")
grip:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Down")
grip:SetScript("OnMouseDown", function() frame:StartSizing("BOTTOMRIGHT") end)
grip:SetScript("OnMouseUp", function()
    frame:StopMovingOrSizing()
    OllamaMonitorDB.compactSize = { frame:GetWidth(), frame:GetHeight() }
end)
grip:Hide()

local sizeButton = CreateFrame("Button", nil, frame)
sizeButton:SetSize(32, 32)
sizeButton:SetHighlightTexture("Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight", "ADD")

local function SetCompact(on)
    M.compact = on
    OllamaMonitorDB = OllamaMonitorDB or {}
    OllamaMonitorDB.compact = on

    for _, region in ipairs(FULL_ONLY) do
        if on then region:Hide() else region:Show() end
    end

    detail:ClearAllPoints()
    textBg:ClearAllPoints()
    close:ClearAllPoints()
    sizeButton:ClearAllPoints()
    if on then
        local size = OllamaMonitorDB.compactSize or { 460, 300 }
        frame:SetSize(size[1], size[2])
        frame:SetBackdrop(COMPACT_BACKDROP)
        frame:SetBackdropColor(0, 0, 0, 0.55)
        frame:SetBackdropBorderColor(0.4, 0.4, 0.4, 0.6)
        detail:SetPoint("TOPLEFT", 8, -6)
        detail:SetPoint("BOTTOMRIGHT", -8, 8)
        textBg:SetPoint("TOPLEFT", header, "BOTTOMLEFT", -4, -2)
        textBg:SetPoint("BOTTOMRIGHT", detail, "BOTTOMRIGHT", 0, 0)
        textBg:SetBackdropColor(0, 0, 0, 0)
        textBg:SetBackdropBorderColor(0, 0, 0, 0)
        close:SetPoint("TOPRIGHT", 2, 2)
        sizeButton:SetPoint("RIGHT", close, "LEFT", 8, 0)
        sizeButton:SetNormalTexture("Interface\\Buttons\\UI-Panel-BiggerButton-Up")
        sizeButton:SetPushedTexture("Interface\\Buttons\\UI-Panel-BiggerButton-Down")
        header:SetPoint("RIGHT", -44, 0)
        grip:Show()
    else
        frame:SetSize(FULL_W, FULL_H)
        frame:SetBackdrop(FULL_BACKDROP)
        frame:SetBackdropColor(0, 0, 0, 0.92)
        detail:SetPoint("TOPLEFT", listPanel, "TOPRIGHT", 8, 0)
        detail:SetPoint("BOTTOMRIGHT", -18, 18)
        textBg:SetPoint("TOPLEFT", tabs[1], "BOTTOMLEFT", 0, -6)
        textBg:SetPoint("BOTTOMRIGHT", 0, 52)
        textBg:SetBackdropColor(0.02, 0.02, 0.04, 0.95)
        textBg:SetBackdropBorderColor(1, 1, 1, 1)
        close:SetPoint("TOPRIGHT", -6, -6)
        sizeButton:SetPoint("RIGHT", close, "LEFT", 8, 0)
        sizeButton:SetNormalTexture("Interface\\Buttons\\UI-Panel-SmallerButton-Up")
        sizeButton:SetPushedTexture("Interface\\Buttons\\UI-Panel-SmallerButton-Down")
        header:SetPoint("RIGHT", -4, 0)
        grip:Hide()
    end
end
M.SetCompact = SetCompact

sizeButton:SetScript("OnClick", function() SetCompact(not M.compact) end)
sizeButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:AddLine(M.compact and "Full window" or "Log only")
    GameTooltip:Show()
end)
sizeButton:SetScript("OnLeave", function() GameTooltip:Hide() end)
SetCompact(false)

-- ---------------------------------------------------------------------------
-- Minimap button: left-click shows or hides the window, right-click switches
-- full / log only, drag moves it around the minimap's edge.
-- ---------------------------------------------------------------------------

local mini = CreateFrame("Button", "OllamaMonitorMinimapButton", Minimap)
mini:SetSize(31, 31)
mini:SetFrameStrata("MEDIUM")
mini:SetFrameLevel(8)
mini:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")
mini:RegisterForClicks("LeftButtonUp", "RightButtonUp")
mini:RegisterForDrag("LeftButton")

local miniIcon = mini:CreateTexture(nil, "BACKGROUND")
miniIcon:SetSize(20, 20)
miniIcon:SetPoint("TOPLEFT", 7, -5)
miniIcon:SetTexture("Interface\\Icons\\INV_Misc_Spyglass_03")
miniIcon:SetTexCoord(0.08, 0.92, 0.08, 0.92)

local miniBorder = mini:CreateTexture(nil, "OVERLAY")
miniBorder:SetSize(53, 53)
miniBorder:SetPoint("TOPLEFT")
miniBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")

local function PlaceMinimapButton()
    local angle = math.rad((OllamaMonitorDB and OllamaMonitorDB.minimapAngle) or 200)
    mini:ClearAllPoints()
    mini:SetPoint("CENTER", Minimap, "CENTER", math.cos(angle) * 80, math.sin(angle) * 80)
end
M.PlaceMinimapButton = PlaceMinimapButton

mini:SetScript("OnDragStart", function(self)
    self:SetScript("OnUpdate", function()
        local mx, my = Minimap:GetCenter()
        local cx, cy = GetCursorPosition()
        local scale = Minimap:GetEffectiveScale()
        OllamaMonitorDB = OllamaMonitorDB or {}
        OllamaMonitorDB.minimapAngle = math.deg(math.atan2(cy / scale - my, cx / scale - mx))
        PlaceMinimapButton()
    end)
end)
mini:SetScript("OnDragStop", function(self) self:SetScript("OnUpdate", nil) end)

mini:SetScript("OnClick", function(self, button)
    if button == "RightButton" then
        SetCompact(not M.compact)
        frame:Show()
    elseif frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
    end
end)
mini:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:AddLine("Ollama Monitor")
    GameTooltip:AddLine("Left-click: show / hide", 0.8, 0.8, 0.8)
    GameTooltip:AddLine("Right-click: full window / log only", 0.8, 0.8, 0.8)
    GameTooltip:AddLine("Drag: move this button", 0.8, 0.8, 0.8)
    if M.watching then
        GameTooltip:AddLine("Camera on " .. (M.watchName or "?"), 0.4, 1, 0.4)
    end
    GameTooltip:Show()
end)
mini:SetScript("OnLeave", function() GameTooltip:Hide() end)
PlaceMinimapButton()

-- ---------------------------------------------------------------------------
-- Events and slash command
-- ---------------------------------------------------------------------------

local events = CreateFrame("Frame")
events:RegisterEvent("CHAT_MSG_ADDON")
events:RegisterEvent("ADDON_LOADED")
events:SetScript("OnEvent", function(self, event, arg1, arg2, arg3, arg4)
    if event == "CHAT_MSG_ADDON" then
        if arg1 == PREFIX and arg4 == UnitName("player") then OnMessage(arg2) end
    elseif event == "ADDON_LOADED" and arg1 == "OllamaMonitor" then
        OllamaMonitorDB = OllamaMonitorDB or {}
        local pt = OllamaMonitorDB.point
        if pt then
            frame:ClearAllPoints()
            frame:SetPoint(pt[1], UIParent, pt[2], pt[3], pt[4])
        end
        M.page = OllamaMonitorDB.page or "overview"
        if OllamaMonitorDB.compact then M.SetCompact(true) end
        M.PlaceMinimapButton()   -- the saved angle has only now been loaded
        self:UnregisterEvent("ADDON_LOADED")
    end
end)

SLASH_OLLAMAMONITOR1 = "/om"
SLASH_OLLAMAMONITOR2 = "/ollamamonitor"
SlashCmdList["OLLAMAMONITOR"] = function(arg)
    arg = string.lower(arg or "")
    if arg == "stop" then
        Send("UNWATCH")
    elseif arg == "pause" then
        OllamaMonitorDB.auto = false
        Print("auto refresh paused (/om resume)")
    elseif arg == "resume" then
        OllamaMonitorDB.auto = true
        Print("auto refresh on")
    elseif arg == "mini" then
        M.SetCompact(not M.compact)
        frame:Show()
    elseif frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
    end
end
