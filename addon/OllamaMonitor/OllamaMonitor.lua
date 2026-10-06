-- Ollama Monitor: a live debug window for mod-ollama-chat's autopilot bots.
--
-- Talks to the server by whispering its own character on the addon channel
-- ("OAPM\t<request>"); the server answers the same way. See
-- src/mod-ollama-chat_monitor.cpp for the protocol. Needs an account at
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

-- Coordinates on Interface\Glues\CharacterCreate\UI-CharacterCreate-Classes.
local CLASS_ICONS = "Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Classes"
local CLASS_COORDS = {
    WARRIOR     = { 0, 0.25, 0, 0.25 },
    MAGE        = { 0.25, 0.49609375, 0, 0.25 },
    ROGUE       = { 0.49609375, 0.7421875, 0, 0.25 },
    DRUID       = { 0.7421875, 0.98828125, 0, 0.25 },
    HUNTER      = { 0, 0.25, 0.25, 0.5 },
    SHAMAN      = { 0.25, 0.49609375, 0.25, 0.5 },
    PRIEST      = { 0.49609375, 0.7421875, 0.25, 0.5 },
    WARLOCK     = { 0.7421875, 0.98828125, 0.25, 0.5 },
    PALADIN     = { 0, 0.25, 0.5, 0.75 },
    DEATHKNIGHT = { 0.25, 0.49609375, 0.5, 0.75 },
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

local TIER_COLORS = {
    foreground = "60e060",
    background = "60a0ff",
    dormant    = "909090",
}

-- Quick filters above the list. A chip matches any of its states.
local CHIPS = {
    { key = "fighting", label = "Fighting", color = "ff4040", states = { ["fighting"] = true } },
    { key = "moving",   label = "Moving",   color = "40c0ff",
      states = { ["travelling"] = true, ["flying"] = true, ["aboard"] = true } },
    { key = "waiting",  label = "Waiting",  color = "ffd040", states = { ["waiting on the model"] = true } },
    { key = "idle",     label = "Idle",     color = "ff9040",
      states = { ["idle"] = true, ["dead"] = true, ["corpse run"] = true } },
}

-- Palette (r, g, b) for the flat frame.
local C = {
    window  = { 0.055, 0.058, 0.075 },
    bar     = { 0.09, 0.095, 0.12 },
    side    = { 0.07, 0.074, 0.095 },
    content = { 0.035, 0.037, 0.048 },
    line    = { 0.20, 0.21, 0.26 },
    accent  = { 0.30, 0.62, 1.00 },
}

local WHITE = "Interface\\Buttons\\WHITE8X8"

local LIST_EVERY = 5
local ROWS       = 14
local ROW_HEIGHT = 34
local FULL_W, FULL_H = 980, 620
local SIDE_W     = 280

local M = {
    bots       = {},     -- sorted list of { guid, name, level, class, zone, tier, state, doing }
    incoming   = nil,    -- list being received
    selected   = nil,    -- guid (string)
    page       = "overview",
    pending    = nil,    -- page being received: { guid, page, lines, partial }
    waiting    = false,  -- a page request is out
    askedAt    = 0,
    pageAt     = 0,
    listAt     = 0,
    helloAt    = 0,
    plain      = false,
    watching   = nil,    -- guid the camera follows
    watchNote  = "",
    allowed    = nil,    -- nil until the server answers HELLO
    active     = nil,    -- autopilot on the server: true / false / nil (unknown)
    offReasons = {},     -- why autopilot is off, from the server
    filter     = "",
    chip       = nil,    -- CHIPS key, or nil for all
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

local function SelectedBot()
    return M.selected and BotByGuid(M.selected)
end

local function ClassColor(class)
    local c = RAID_CLASS_COLORS[CLASS_FILES[class] or ""]
    if not c then return "ffffff" end
    return string.format("%02x%02x%02x", c.r * 255, c.g * 255, c.b * 255)
end

local function ClassName(class)
    local file = CLASS_FILES[class]
    if not file then return "" end
    return (LOCALIZED_CLASS_NAMES_MALE and LOCALIZED_CLASS_NAMES_MALE[file]) or file
end

local function SetClassIcon(tex, class)
    local coords = CLASS_COORDS[CLASS_FILES[class] or ""]
    if coords then
        tex:SetTexture(CLASS_ICONS)
        tex:SetTexCoord(coords[1], coords[2], coords[3], coords[4])
    else
        tex:SetTexture("Interface\\Icons\\INV_Misc_QuestionMark")
        tex:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    end
end

local function Hex(hex)
    return tonumber(string.sub(hex, 1, 2), 16) / 255, tonumber(string.sub(hex, 3, 4), 16) / 255,
        tonumber(string.sub(hex, 5, 6), 16) / 255
end

local function StateColor(state) return STATE_COLORS[state] or "c0c0c0" end

-- A flat panel: solid fill and a one-pixel border.
local function Flat(f, fill, alpha, border)
    f:SetBackdrop({ bgFile = WHITE, edgeFile = WHITE, edgeSize = 1,
                    insets = { left = 0, right = 0, top = 0, bottom = 0 } })
    f:SetBackdropColor(fill[1], fill[2], fill[3], alpha or 1)
    local b = border or C.line
    f:SetBackdropBorderColor(b[1], b[2], b[3], 1)
end

local function Line(parent, layer)
    local t = parent:CreateTexture(nil, layer or "ARTWORK")
    t:SetTexture(C.line[1], C.line[2], C.line[3], 1)
    return t
end

local function Tooltip(widget, title, body)
    widget:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_TOP")
        GameTooltip:AddLine(title, 1, 1, 1)
        if body then GameTooltip:AddLine(body, 0.8, 0.8, 0.8, true) end
        GameTooltip:Show()
    end)
    widget:SetScript("OnLeave", function() GameTooltip:Hide() end)
end

-- An edit box with grey hint text while empty.
local function Placeholder(box, hint)
    local fs = box:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    fs:SetPoint("LEFT", 2, 0)
    fs:SetText(hint)
    box.hint = fs
    local function Update()
        if (box:GetText() or "") == "" and not box.focused then fs:Show() else fs:Hide() end
    end
    box:HookScript("OnEditFocusGained", function() box.focused = true; Update() end)
    box:HookScript("OnEditFocusLost", function() box.focused = false; Update() end)
    box:HookScript("OnTextChanged", Update)
    box.UpdateHint = Update
    Update()
end

-- ---------------------------------------------------------------------------
-- Window
-- ---------------------------------------------------------------------------

local frame = CreateFrame("Frame", "OllamaMonitorFrame", UIParent)
frame:SetSize(FULL_W, FULL_H)
frame:SetPoint("CENTER")
frame:SetFrameStrata("HIGH")
frame:SetToplevel(true)
frame:EnableMouse(true)
frame:SetMovable(true)
frame:SetResizable(true)
frame:SetClampedToScreen(true)
frame:Hide()
tinsert(UISpecialFrames, "OllamaMonitorFrame")   -- Escape closes it

local function SavePosition()
    local point, _, relPoint, x, y = frame:GetPoint()
    OllamaMonitorDB = OllamaMonitorDB or {}
    OllamaMonitorDB.point = { point, relPoint, x, y }
end

-- Title bar: drag handle, title, server state.
local titleBar = CreateFrame("Frame", nil, frame)
titleBar:SetPoint("TOPLEFT", 1, -1)
titleBar:SetPoint("TOPRIGHT", -1, -1)
titleBar:SetHeight(30)
titleBar:EnableMouse(true)
titleBar:RegisterForDrag("LeftButton")
titleBar:SetScript("OnDragStart", function() frame:StartMoving() end)
titleBar:SetScript("OnDragStop", function() frame:StopMovingOrSizing(); SavePosition() end)
do
    local bg = titleBar:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints()
    bg:SetTexture(C.bar[1], C.bar[2], C.bar[3], 1)
    local accent = titleBar:CreateTexture(nil, "ARTWORK")
    accent:SetPoint("BOTTOMLEFT")
    accent:SetPoint("BOTTOMRIGHT")
    accent:SetHeight(1)
    accent:SetTexture(C.accent[1], C.accent[2], C.accent[3], 0.6)
end

local titleIcon = titleBar:CreateTexture(nil, "ARTWORK")
titleIcon:SetSize(18, 18)
titleIcon:SetPoint("LEFT", 10, 0)
titleIcon:SetTexture("Interface\\Icons\\INV_Misc_Spyglass_03")
titleIcon:SetTexCoord(0.08, 0.92, 0.08, 0.92)

local title = titleBar:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("LEFT", titleIcon, "RIGHT", 8, 0)
title:SetText("Ollama Monitor")

-- Server state pill: autopilot on / off / connecting.
local pill = CreateFrame("Frame", nil, titleBar)
pill:SetHeight(18)
pill:SetPoint("LEFT", title, "RIGHT", 12, 0)
local pillText = pill:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
pillText:SetPoint("CENTER")

local function SetPill(label, hex)
    local r, g, b = Hex(hex)
    Flat(pill, { r * 0.25, g * 0.25, b * 0.25 }, 1, { r * 0.7, g * 0.7, b * 0.7 })
    pillText:SetText("|cff" .. hex .. label .. "|r")
    pill:SetWidth(pillText:GetStringWidth() + 18)
end
SetPill("CONNECTING", "a0a0a0")

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetSize(28, 28)
close:SetPoint("TOPRIGHT", 0, 0)

local sizeButton = CreateFrame("Button", nil, frame)
sizeButton:SetSize(28, 28)
sizeButton:SetPoint("RIGHT", close, "LEFT", 4, 0)
sizeButton:SetHighlightTexture("Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight", "ADD")

-- Banner under the title bar: why autopilot is off, or why we may not look.
local banner = CreateFrame("Frame", nil, frame)
banner:SetPoint("TOPLEFT", titleBar, "BOTTOMLEFT", 0, 0)
banner:SetPoint("TOPRIGHT", titleBar, "BOTTOMRIGHT", 0, 0)
banner:SetHeight(24)
do
    local bg = banner:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints()
    bg:SetTexture(0.32, 0.07, 0.07, 1)
end
local bannerIcon = banner:CreateTexture(nil, "ARTWORK")
bannerIcon:SetSize(14, 14)
bannerIcon:SetPoint("TOPLEFT", 10, -5)
bannerIcon:SetTexture("Interface\\DialogFrame\\UI-Dialog-Icon-AlertNew")
local bannerText = banner:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
bannerText:SetPoint("TOPLEFT", bannerIcon, "TOPRIGHT", 8, 0)
bannerText:SetPoint("RIGHT", -10, 0)
bannerText:SetJustifyH("LEFT")
bannerText:SetJustifyV("TOP")
banner:Hide()

-- Everything under the title bar (and the banner, when shown).
local body = CreateFrame("Frame", nil, frame)
body:SetPoint("BOTTOMRIGHT", -1, 1)

local function LayoutBody()
    body:ClearAllPoints()
    body:SetPoint("BOTTOMRIGHT", -1, 1)
    if banner:IsShown() then
        body:SetPoint("TOPLEFT", banner, "BOTTOMLEFT", 0, 0)
    else
        body:SetPoint("TOPLEFT", titleBar, "BOTTOMLEFT", 0, 0)
    end
end

local function ShowBanner(lines)
    if not lines or #lines == 0 or M.compact then
        banner:Hide()
    else
        bannerText:SetText(table.concat(lines, "\n"))
        banner:SetHeight(math.max(24, bannerText:GetStringHeight() + 12))
        banner:Show()
    end
    LayoutBody()
end

-- ---------------------------------------------------------------------------
-- Sidebar: search, quick filters, bot list
-- ---------------------------------------------------------------------------

local side = CreateFrame("Frame", nil, body)
side:SetPoint("TOPLEFT")
side:SetPoint("BOTTOMLEFT")
side:SetWidth(SIDE_W)
do
    local bg = side:CreateTexture(nil, "BACKGROUND")
    bg:SetAllPoints()
    bg:SetTexture(C.side[1], C.side[2], C.side[3], 1)
    local edge = Line(side)
    edge:SetPoint("TOPRIGHT")
    edge:SetPoint("BOTTOMRIGHT")
    edge:SetWidth(1)
end

local filterBox = CreateFrame("EditBox", "OllamaMonitorFilter", side, "InputBoxTemplate")
filterBox:SetHeight(22)
filterBox:SetPoint("TOPLEFT", 18, -10)
filterBox:SetPoint("TOPRIGHT", -12, -10)
filterBox:SetAutoFocus(false)
filterBox:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
filterBox:SetScript("OnEnterPressed", function(self) self:ClearFocus() end)
Placeholder(filterBox, "Search name, zone, state or task")

-- Quick filters.
local chips = {}
local RefreshList

local function UpdateChips()
    local counts = {}
    for _, chip in ipairs(CHIPS) do counts[chip.key] = 0 end
    for _, b in ipairs(M.bots) do
        for _, chip in ipairs(CHIPS) do
            if chip.states[b.state] then counts[chip.key] = counts[chip.key] + 1 end
        end
    end
    for _, c in ipairs(chips) do
        local chip = c.chip
        local on = M.chip == chip.key
        local r, g, b = Hex(chip.color)
        if on then
            Flat(c, { r * 0.3, g * 0.3, b * 0.3 }, 1, { r, g, b })
        else
            Flat(c, C.window, 1, C.line)
        end
        c.text:SetText(string.format("|cff%s%d|r %s", chip.color, counts[chip.key], chip.label))
    end
end

do
    local chipW = math.floor((SIDE_W - 24 - 3 * 4) / 4)
    for i, chip in ipairs(CHIPS) do
        local c = CreateFrame("Button", nil, side)
        c:SetSize(chipW, 20)
        if i == 1 then
            c:SetPoint("TOPLEFT", 12, -40)
        else
            c:SetPoint("LEFT", chips[i - 1], "RIGHT", 4, 0)
        end
        c.text = c:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        c.text:SetPoint("CENTER")
        c.chip = chip
        local hl = c:CreateTexture(nil, "HIGHLIGHT")
        hl:SetAllPoints()
        hl:SetTexture(1, 1, 1, 0.06)
        c:SetScript("OnClick", function()
            M.chip = (M.chip ~= chip.key) and chip.key or nil
            UpdateChips()
            RefreshList()
        end)
        Tooltip(c, chip.label, "Show only these bots. Click again to show all.")
        chips[i] = c
    end
end

local listTop = -68
local listScroll = CreateFrame("ScrollFrame", "OllamaMonitorListScroll", side, "FauxScrollFrameTemplate")
listScroll:SetPoint("TOPLEFT", 0, listTop)
listScroll:SetPoint("BOTTOMRIGHT", -26, 30)

local countText = side:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
countText:SetPoint("BOTTOMLEFT", 12, 10)

local refreshButton = CreateFrame("Button", nil, side)
refreshButton:SetSize(16, 16)
refreshButton:SetPoint("BOTTOMRIGHT", -10, 8)
refreshButton:SetNormalTexture("Interface\\Buttons\\UI-RefreshButton")
refreshButton:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")
refreshButton:SetScript("OnClick", function() M.listAt = 0 end)
Tooltip(refreshButton, "Refresh the list", "It also refreshes on its own every few seconds.")

-- Shown when the list has nothing to show.
local listEmpty = side:CreateFontString(nil, "OVERLAY", "GameFontDisable")
listEmpty:SetPoint("TOPLEFT", 16, listTop - 16)
listEmpty:SetPoint("RIGHT", -16, 0)
listEmpty:SetJustifyH("LEFT")
listEmpty:SetJustifyV("TOP")

local function Visible()
    local out, needle = {}, string.lower(M.filter)
    local chip
    for _, c in ipairs(CHIPS) do
        if c.key == M.chip then chip = c end
    end
    for _, b in ipairs(M.bots) do
        local ok = not chip or chip.states[b.state]
        if ok and needle ~= "" then
            local hay = string.lower(b.name .. " " .. b.zone .. " " .. b.state .. " " .. b.doing)
            ok = string.find(hay, needle, 1, true) ~= nil
        end
        if ok then table.insert(out, b) end
    end
    return out
end

local rows = {}
local SelectBot

for i = 1, ROWS do
    local row = CreateFrame("Button", nil, side)
    row:SetHeight(ROW_HEIGHT)
    row:SetPoint("TOPLEFT", listScroll, "TOPLEFT", 0, -(i - 1) * ROW_HEIGHT)
    row:SetPoint("RIGHT", listScroll, "RIGHT", 0, 0)

    local hl = row:CreateTexture(nil, "HIGHLIGHT")
    hl:SetAllPoints()
    hl:SetTexture(1, 1, 1, 0.05)

    row.sel = row:CreateTexture(nil, "BACKGROUND")
    row.sel:SetAllPoints()
    row.sel:SetTexture(C.accent[1], C.accent[2], C.accent[3], 0.16)
    row.bar = row:CreateTexture(nil, "ARTWORK")
    row.bar:SetPoint("TOPLEFT")
    row.bar:SetPoint("BOTTOMLEFT")
    row.bar:SetWidth(3)
    row.bar:SetTexture(C.accent[1], C.accent[2], C.accent[3], 1)

    row.icon = row:CreateTexture(nil, "ARTWORK")
    row.icon:SetSize(24, 24)
    row.icon:SetPoint("LEFT", 10, 0)

    row.name = row:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    row.name:SetPoint("TOPLEFT", row.icon, "TOPRIGHT", 8, 1)
    row.name:SetJustifyH("LEFT")

    row.level = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    row.level:SetPoint("TOPRIGHT", -6, -5)

    row.dot = row:CreateTexture(nil, "ARTWORK")
    row.dot:SetSize(6, 6)
    row.dot:SetPoint("BOTTOMLEFT", row.icon, "BOTTOMRIGHT", 8, 2)
    row.dot:SetTexture(WHITE)

    row.sub = row:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    row.sub:SetPoint("LEFT", row.dot, "RIGHT", 5, 0)
    row.sub:SetPoint("RIGHT", -6, 0)
    row.sub:SetHeight(12)
    row.sub:SetJustifyH("LEFT")

    local divider = row:CreateTexture(nil, "BORDER")
    divider:SetPoint("BOTTOMLEFT", 8, 0)
    divider:SetPoint("BOTTOMRIGHT", -4, 0)
    divider:SetHeight(1)
    divider:SetTexture(1, 1, 1, 0.04)

    row:SetScript("OnClick", function(self)
        if self.guid then SelectBot(self.guid) end
    end)
    row:SetScript("OnEnter", function(self)
        local b = self.guid and BotByGuid(self.guid)
        if not b then return end
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine(string.format("|cff%s%s|r", ClassColor(b.class), b.name))
        GameTooltip:AddLine("Level " .. b.level .. " " .. ClassName(b.class), 0.8, 0.8, 0.8)
        GameTooltip:AddLine(b.zone, 0.8, 0.8, 0.8)
        GameTooltip:AddDoubleLine("State", b.state, 0.6, 0.6, 0.6, Hex(StateColor(b.state)))
        GameTooltip:AddDoubleLine("Tier", b.tier, 0.6, 0.6, 0.6, Hex(TIER_COLORS[b.tier] or "c0c0c0"))
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
            SetClassIcon(row.icon, b.class)
            row.name:SetText(string.format("|cff%s%s|r", ClassColor(b.class), b.name))
            row.level:SetText(b.level)
            row.dot:SetVertexColor(Hex(StateColor(b.state)))
            row.sub:SetText(string.format("|cff%s%s|r%s", StateColor(b.state), b.state,
                b.doing ~= "" and ("  |cff808080" .. b.doing .. "|r") or ""))
            if b.guid == M.selected then row.sel:Show(); row.bar:Show() else row.sel:Hide(); row.bar:Hide() end
            row:Show()
        else
            -- A row vanishing under the cursor never gets OnLeave.
            if GameTooltip:GetOwner() == row then GameTooltip:Hide() end
            row.guid = nil
            row:Hide()
        end
    end

    if #M.bots == 0 then
        listEmpty:SetText(M.allowed
            and "No bots on autopilot.\n\n|cff909090Target a bot and press Turn on, or let the server's OllamaChat.Autopilot.Select rules pick some.|r"
            or "|cff909090Waiting for the server...|r")
        listEmpty:Show()
    elseif #list == 0 then
        listEmpty:SetText("No bot matches.\n\n|cff909090Clear the search or the filter.|r")
        listEmpty:Show()
    else
        listEmpty:Hide()
    end
    countText:SetText(#list == #M.bots and (#M.bots .. " bots") or (#list .. " of " .. #M.bots .. " bots"))
    UpdateChips()
end

listScroll:SetScript("OnVerticalScroll", function(self, offset)
    FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, RefreshList)
end)
-- Hooked, not set: SetScript would drop the placeholder's own hook.
filterBox:HookScript("OnTextChanged", function(self)
    M.filter = self:GetText() or ""
    RefreshList()
end)

-- ---------------------------------------------------------------------------
-- Main pane: bot card, actions, tabs, page, footer
-- ---------------------------------------------------------------------------

local main = CreateFrame("Frame", nil, body)
main:SetPoint("TOPLEFT", side, "TOPRIGHT", 0, 0)
main:SetPoint("BOTTOMRIGHT")

-- Bot card.
local card = CreateFrame("Frame", nil, main)
card:SetPoint("TOPLEFT", 14, -12)
card:SetPoint("TOPRIGHT", -14, -12)
card:SetHeight(52)

local cardIcon = card:CreateTexture(nil, "ARTWORK")
cardIcon:SetSize(44, 44)
cardIcon:SetPoint("LEFT", 0, 0)

local cardName = card:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
cardName:SetPoint("TOPLEFT", cardIcon, "TOPRIGHT", 12, -1)
cardName:SetJustifyH("LEFT")

local cardSub = card:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
cardSub:SetPoint("TOPLEFT", cardName, "BOTTOMLEFT", 0, -3)
cardSub:SetJustifyH("LEFT")

local cardDoing = card:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
cardDoing:SetPoint("TOPLEFT", cardSub, "BOTTOMLEFT", 0, -3)
cardDoing:SetPoint("RIGHT", card, "RIGHT", -150, 0)
cardDoing:SetHeight(12)
cardDoing:SetJustifyH("LEFT")

local statePill = CreateFrame("Frame", nil, card)
statePill:SetHeight(20)
statePill:SetPoint("TOPRIGHT", 0, -2)
local statePillText = statePill:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
statePillText:SetPoint("CENTER")

local tierText = card:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
tierText:SetPoint("TOPRIGHT", statePill, "BOTTOMRIGHT", 0, -6)

-- Action bar.
local actions = CreateFrame("Frame", nil, main)
actions:SetPoint("TOPLEFT", card, "BOTTOMLEFT", 0, -10)
actions:SetPoint("TOPRIGHT", card, "BOTTOMRIGHT", 0, -10)
actions:SetHeight(24)

local function MakeButton(parent, label, width, tipTitle, tipBody, onClick)
    local b = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    b:SetSize(width, 24)
    b:SetText(label)
    b:SetScript("OnClick", onClick)
    Tooltip(b, tipTitle, tipBody)
    return b
end

local followButton = MakeButton(actions, "Follow camera", 116, "Follow camera",
    "Your camera follows the bot. Your own character stays where it is.", function()
        if M.watching then
            Send("UNWATCH")
        elseif M.selected then
            Send("WATCH " .. M.selected)
        end
    end)
followButton:SetPoint("LEFT", 0, 0)

local gotoButton = MakeButton(actions, "Teleport to", 96, "Teleport to", "Moves your character to the bot.",
    function() if M.selected then Send("GOTO " .. M.selected) end end)
gotoButton:SetPoint("LEFT", followButton, "RIGHT", 4, 0)

local actionGap = actions:CreateTexture(nil, "ARTWORK")
actionGap:SetSize(1, 18)
actionGap:SetPoint("LEFT", gotoButton, "RIGHT", 8, 0)
actionGap:SetTexture(C.line[1], C.line[2], C.line[3], 1)

local replanButton = MakeButton(actions, "Replan now", 96, "Replan now",
    "Asks the model for new orders for this bot straight away.", function()
        local b = SelectedBot()
        if b then Send("CMD replan " .. b.name) end
    end)
replanButton:SetPoint("LEFT", actionGap, "RIGHT", 8, 0)

local statusButton = MakeButton(actions, "Status to chat", 110, "Status to chat",
    "Prints the bot's full autopilot status in your chat window.", function()
        local b = SelectedBot()
        if b then Send("CMD status " .. b.name) end
    end)
statusButton:SetPoint("LEFT", replanButton, "RIGHT", 4, 0)

-- Taking a bot off autopilot hands its strategies back to what it had
-- before, so ask first.
StaticPopupDialogs["OLLAMAMONITOR_TURN_OFF"] = {
    text = "Take %s off autopilot?\n\nIts strategies go back to what it had before autopilot, and the LLM stops giving it orders.",
    button1 = YES,
    button2 = NO,
    OnAccept = function(self, name)
        Send("CMD off " .. name)
        M.listAt = 0   -- refresh the list soon: the bot leaves it
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

-- Who "Turn on" means: the bot last selected here (it leaves the list once it
-- is off autopilot, but its name is kept), else your current target.
local function TurnOnName()
    if M.selectedName then return M.selectedName end
    if UnitExists("target") and UnitIsPlayer("target") then return UnitName("target") end
    return nil
end

-- One button, two jobs: Turn off while the selected bot is on autopilot,
-- Turn on once it is not.
local onOffButton = MakeButton(actions, "Turn off", 96, "Turn autopilot on or off",
    "Turn off hands the bot back to its own strategies. Turn on puts the selected bot, or your target, on autopilot.",
    function()
        local b = SelectedBot()
        if b then
            local dialog = StaticPopup_Show("OLLAMAMONITOR_TURN_OFF", b.name)
            if dialog then dialog.data = b.name end
            return
        end
        local name = TurnOnName()
        if not name then
            Print("select a bot, or target one, to turn autopilot on")
            return
        end
        Send("CMD on " .. name)
        M.listAt = 0   -- refresh the list soon: the bot joins it
    end)
onOffButton:SetPoint("RIGHT", 0, 0)

-- Tabs.
local tabBar = CreateFrame("Frame", nil, main)
tabBar:SetPoint("TOPLEFT", actions, "BOTTOMLEFT", 0, -12)
tabBar:SetPoint("TOPRIGHT", actions, "BOTTOMRIGHT", 0, -12)
tabBar:SetHeight(26)
do
    local base = Line(tabBar)
    base:SetPoint("BOTTOMLEFT")
    base:SetPoint("BOTTOMRIGHT")
    base:SetHeight(1)
end

local tabs = {}
local ShowPage
for i, p in ipairs(PAGES) do
    local t = CreateFrame("Button", nil, tabBar)
    t:SetSize(84, 26)
    if i == 1 then
        t:SetPoint("BOTTOMLEFT", 0, 0)
    else
        t:SetPoint("LEFT", tabs[i - 1], "RIGHT", 2, 0)
    end
    t.text = t:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    t.text:SetPoint("CENTER", 0, 1)
    t.text:SetText(p.label)
    t.fill = t:CreateTexture(nil, "BACKGROUND")
    t.fill:SetAllPoints()
    t.fill:SetTexture(C.accent[1], C.accent[2], C.accent[3], 0.10)
    t.line = t:CreateTexture(nil, "ARTWORK")
    t.line:SetPoint("BOTTOMLEFT")
    t.line:SetPoint("BOTTOMRIGHT")
    t.line:SetHeight(2)
    t.line:SetTexture(C.accent[1], C.accent[2], C.accent[3], 1)
    local hl = t:CreateTexture(nil, "HIGHLIGHT")
    hl:SetAllPoints()
    hl:SetTexture(1, 1, 1, 0.05)
    t:SetScript("OnClick", function() ShowPage(p.key) end)
    t.key = p.key
    tabs[i] = t
end

local updatedText = tabBar:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
updatedText:SetPoint("RIGHT", 0, 2)

-- Footer: camera, whisper, refresh and text toggles.
local footer = CreateFrame("Frame", nil, main)
footer:SetPoint("BOTTOMLEFT", 14, 10)
footer:SetPoint("BOTTOMRIGHT", -14, 10)
footer:SetHeight(54)

local cameraDot = footer:CreateTexture(nil, "ARTWORK")
cameraDot:SetSize(8, 8)
cameraDot:SetPoint("TOPLEFT", 2, -8)
cameraDot:SetTexture(WHITE)

local cameraText = footer:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
cameraText:SetPoint("LEFT", cameraDot, "RIGHT", 6, 0)
cameraText:SetPoint("RIGHT", footer, "RIGHT", -230, 0)
cameraText:SetHeight(12)
cameraText:SetJustifyH("LEFT")

local function MakeCheck(name, label, tipTitle, tipBody, onClick)
    local c = CreateFrame("CheckButton", name, footer, "UICheckButtonTemplate")
    c:SetSize(22, 22)
    _G[name .. "Text"]:SetText(label)
    _G[name .. "Text"]:SetFontObject(GameFontHighlightSmall)
    c:SetScript("OnClick", onClick)
    Tooltip(c, tipTitle, tipBody)
    return c
end

local autoCheck = MakeCheck("OllamaMonitorAuto", "Auto refresh", "Auto refresh",
    "Keep the page up to date on its own. Off freezes it, so you can read or copy.", function(self)
        OllamaMonitorDB = OllamaMonitorDB or {}
        OllamaMonitorDB.auto = self:GetChecked() and true or false
    end)
autoCheck:SetPoint("TOPRIGHT", footer, "TOPRIGHT", -150, 2)

local plainCheck = MakeCheck("OllamaMonitorPlain", "Plain text", "Plain text",
    "No colours, so text copies cleanly (select it, then Ctrl+C).", function(self)
        M.plain = self:GetChecked() and true or false
        M.Render(true)
    end)
plainCheck:SetPoint("TOPRIGHT", footer, "TOPRIGHT", -60, 2)

local sayBox = CreateFrame("EditBox", "OllamaMonitorSay", footer, "InputBoxTemplate")
sayBox:SetHeight(24)
sayBox:SetPoint("BOTTOMLEFT", 6, 0)
sayBox:SetPoint("BOTTOMRIGHT", -76, 0)
sayBox:SetAutoFocus(false)
sayBox:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
Placeholder(sayBox, "Select a bot to whisper it")

local function SendWhisper()
    local b = SelectedBot()
    local msg = sayBox:GetText()
    if b and msg and msg ~= "" then
        SendChatMessage(msg, "WHISPER", nil, b.name)
        sayBox:SetText("")
        M.pageAt = 0   -- the Chat page shows the answer soonest
    end
end
sayBox:SetScript("OnEnterPressed", function(self) SendWhisper(); self:ClearFocus() end)

local sendButton = MakeButton(footer, "Whisper", 70, "Whisper",
    "Whispers the bot as a player would. Its answer shows on the Chat page.", SendWhisper)
sendButton:SetPoint("BOTTOMRIGHT", 0, 0)

-- The page.
local textBg = CreateFrame("Frame", nil, main)
textBg:SetPoint("TOPLEFT", tabBar, "BOTTOMLEFT", 0, -8)
textBg:SetPoint("BOTTOMRIGHT", footer, "TOPRIGHT", 0, 8)
Flat(textBg, C.content, 1, C.line)

local scroll = CreateFrame("ScrollFrame", "OllamaMonitorDetailScroll", textBg, "UIPanelScrollFrameTemplate")
scroll:SetPoint("TOPLEFT", 10, -10)
scroll:SetPoint("BOTTOMRIGHT", -28, 10)

local text = CreateFrame("EditBox", nil, scroll)
text:SetMultiLine(true)
text:SetAutoFocus(false)
text:SetMaxLetters(0)
text:SetFontObject(ChatFontNormal)
text:SetWidth(560)
text:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
-- Read only, but selectable so lines can be copied out (Ctrl+C).
text:SetScript("OnTextChanged", function(self, userInput)
    if userInput and self.shown then self:SetText(self.shown) end
end)
scroll:SetScrollChild(text)
scroll:SetScript("OnSizeChanged", function(self, width) text:SetWidth(math.max(100, width - 4)) end)

-- In place of the page while no bot is picked.
local emptyPane = CreateFrame("Frame", nil, textBg)
emptyPane:SetAllPoints()
local emptyIcon = emptyPane:CreateTexture(nil, "ARTWORK")
emptyIcon:SetSize(48, 48)
emptyIcon:SetPoint("CENTER", 0, 40)
emptyIcon:SetTexture("Interface\\Icons\\INV_Misc_Spyglass_03")
emptyIcon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
emptyIcon:SetAlpha(0.6)
local emptyTitle = emptyPane:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
emptyTitle:SetPoint("TOP", emptyIcon, "BOTTOM", 0, -14)
local emptyBody = emptyPane:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
emptyBody:SetPoint("TOP", emptyTitle, "BOTTOM", 0, -8)
emptyBody:SetWidth(420)
emptyBody:SetTextColor(0.7, 0.7, 0.7)

-- One line for log-only mode, in place of the card.
local compactHeader = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
compactHeader:SetPoint("TOPLEFT", 10, -8)
compactHeader:SetPoint("RIGHT", -60, 0)
compactHeader:SetJustifyH("LEFT")
compactHeader:Hide()

-- ---------------------------------------------------------------------------
-- Rendering
-- ---------------------------------------------------------------------------

local function Colorize(line)
    if M.plain then return line end
    if string.sub(line, 1, 2) == "# " then
        return "|cffffd100" .. string.upper(string.sub(line, 3)) .. "|r"
    end
    local key, rest = string.match(line, "^(%s*[%w%s'%-%.%(%)]-):( .*)$")
    if key and #key <= 40 then
        local lower = string.lower(line)
        local valueColor = ""
        if string.find(lower, "fail", 1, true) or string.find(lower, "stuck", 1, true) or
           string.find(lower, "error", 1, true) or string.find(lower, "dropped", 1, true) or
           string.find(lower, "idle:", 1, true) then
            valueColor = "|cffff6060"
        end
        return "|cff80b8ff" .. key .. ":|r" .. valueColor .. rest .. (valueColor ~= "" and "|r" or "")
    end
    return line
end

M.lines = {}

function M.Render(keepScroll)
    local out = {}
    for _, line in ipairs(M.lines) do
        -- A blank line before each section heading, so sections stand apart.
        if string.sub(line, 1, 2) == "# " and #out > 0 then table.insert(out, "") end
        table.insert(out, Colorize(line))
    end
    local shown = table.concat(out, "\n")
    local saved = scroll:GetVerticalScroll()
    text.shown = shown
    text:SetText(shown)
    if keepScroll then
        -- The edit box lays the text out on the next frame; scroll after it.
        M.restoreScroll = saved
    else
        scroll:SetVerticalScroll(0)
    end
end

local function SetStatePill(state)
    local hex = StateColor(state)
    local r, g, b = Hex(hex)
    Flat(statePill, { r * 0.22, g * 0.22, b * 0.22 }, 1, { r * 0.7, g * 0.7, b * 0.7 })
    statePillText:SetText("|cff" .. hex .. string.upper(state) .. "|r")
    statePill:SetWidth(statePillText:GetStringWidth() + 20)
end

local function SetEnabled(button, on)
    if on then button:Enable() else button:Disable() end
end

local function UpdateHeader()
    local b = SelectedBot()
    onOffButton:SetText(b and "Turn off" or "Turn on")
    SetEnabled(followButton, b ~= nil or M.watching ~= nil)
    SetEnabled(gotoButton, b ~= nil)
    SetEnabled(replanButton, b ~= nil)
    SetEnabled(statusButton, b ~= nil)
    SetEnabled(sendButton, b ~= nil)
    sayBox.hint:SetText(b and ("Whisper " .. b.name .. " as a player would") or "Select a bot to whisper it")

    if b then
        SetClassIcon(cardIcon, b.class)
        cardName:SetText(string.format("|cff%s%s|r", ClassColor(b.class), b.name))
        cardSub:SetText(string.format("Level %d %s  |cff606060|||r  %s", b.level, ClassName(b.class), b.zone))
        cardDoing:SetText(b.doing ~= "" and b.doing or "|cff808080no current task|r")
        SetStatePill(b.state)
        statePill:Show()
        tierText:SetText("tier |cff" .. (TIER_COLORS[b.tier] or "c0c0c0") .. b.tier .. "|r")
        compactHeader:SetText(string.format("|cff%s%s|r  |cff909090%d  %s|r  |cff%s%s|r", ClassColor(b.class),
            b.name, b.level, b.zone, StateColor(b.state), b.state))
        emptyPane:Hide()
        scroll:Show()
        return
    end

    statePill:Hide()
    tierText:SetText("")
    cardIcon:SetTexture("Interface\\Icons\\INV_Misc_Spyglass_03")
    cardIcon:SetTexCoord(0.08, 0.92, 0.08, 0.92)
    cardDoing:SetText("")
    if M.selected then
        -- It was picked, but has left autopilot or logged out: keep its last
        -- page readable.
        local name = M.selectedName or "That bot"
        cardName:SetText("|cffa0a0a0" .. name .. "|r")
        cardSub:SetText("|cff909090Not on autopilot, or not online. Turn on puts it back.|r")
        compactHeader:SetText("|cffa0a0a0" .. name .. " - not on autopilot or offline|r")
        emptyPane:Hide()
        scroll:Show()
    else
        cardName:SetText("No bot selected")
        cardSub:SetText("|cff909090Pick one on the left, or target a bot and press Turn on.|r")
        compactHeader:SetText("|cffa0a0a0No bot selected|r")
        emptyTitle:SetText(#M.bots > 0 and "Pick a bot" or "No bots on autopilot yet")
        emptyBody:SetText(#M.bots > 0
            and "Choose a bot on the left to see what it is doing, follow it with the camera, or read what the model told it."
            or "Target a bot and press Turn on to put it on autopilot. Bots the server's rules enroll show up here on their own.")
        emptyPane:Show()
        scroll:Hide()
    end
end

local function UpdateCamera()
    if M.watching then
        local b = BotByGuid(M.watching)
        followButton:SetText("Stop camera")
        cameraDot:SetVertexColor(0.3, 1, 0.3)
        cameraText:SetText("Camera on |cffffffff" .. ((b and b.name) or M.watchName or "?") .. "|r" ..
            (M.watchNote ~= "" and ("  |cff909090" .. M.watchNote .. "|r") or ""))
    else
        followButton:SetText("Follow camera")
        cameraDot:SetVertexColor(0.4, 0.4, 0.4)
        cameraText:SetText("|cff909090Camera: your own" ..
            (M.watchNote ~= "" and ("  " .. M.watchNote) or "") .. "|r")
    end
    SetEnabled(followButton, SelectedBot() ~= nil or M.watching ~= nil)
end

local function UpdateTabs()
    for _, t in ipairs(tabs) do
        if t.key == M.page then
            t.text:SetTextColor(1, 1, 1)
            t.line:Show()
            t.fill:Show()
        else
            t.text:SetTextColor(0.6, 0.6, 0.65)
            t.line:Hide()
            t.fill:Hide()
        end
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
    M.gone = false
    local picked = BotByGuid(guid)
    if picked then M.selectedName = picked.name end
    M.lines = { "loading..." }
    M.Render(false)
    updatedText:SetText("")
    UpdateHeader()
    UpdateCamera()
    RefreshList()
    RequestPage()
end

local function UpdateServerState()
    if M.allowed and M.active then
        SetPill("AUTOPILOT ON", "60e060")
        ShowBanner(nil)
    elseif M.allowed and M.active == false then
        SetPill("AUTOPILOT OFF", "ff6060")
        local lines = {}
        for _, why in ipairs(M.offReasons) do table.insert(lines, why) end
        if #lines == 0 then lines = { "Autopilot is off on the server (.ollama autopilot status says why)." } end
        ShowBanner(lines)
    elseif M.denied then
        SetPill("NO ACCESS", "ff6060")
        ShowBanner({ M.denied })
    elseif M.noAnswer then
        SetPill("NO ANSWER", "ffa040")
        ShowBanner({ "The server does not answer. Is mod-ollama-chat installed, with OllamaChat.Monitor.Enable = 1?" })
    else
        SetPill("CONNECTING", "a0a0a0")
        ShowBanner(nil)
    end
end

-- ---------------------------------------------------------------------------
-- Messages from the server
-- ---------------------------------------------------------------------------

local function OnMessage(msg)
    local f = Split(msg)
    local kind = f[1]

    if kind == "H" then
        M.allowed = true
        M.noAnswer = false
        M.denied = nil
        M.active = f[3] == "active"
        M.offReasons = {}
        UpdateServerState()
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
        -- Back online: pick up where the page left off.
        if M.gone and BotByGuid(M.selected) then
            M.gone = false
            M.pageAt = 0
        end
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
    elseif kind == "G" then
        -- The selected bot logged out. Stop asking; keep its last page up,
        -- marked, so what it was doing can still be read.
        M.waiting = false
        if f[2] == M.selected and not M.gone then
            M.gone = true
            table.insert(M.lines, 1, "# OFFLINE - last known state below; this page resumes if it logs back in")
            M.Render(true)
            updatedText:SetText("offline since " .. date("%H:%M:%S"))
        end
    elseif kind == "M" then
        M.waiting = false
        local text = f[2] or ""
        local why = string.match(text, "^Autopilot is off: (.*)$")
        if why then
            table.insert(M.offReasons, why)
            UpdateServerState()
        elseif not M.allowed then
            -- The server refused us (MinSecurity, or the monitor turned off).
            M.denied = text
            M.noAnswer = false
            UpdateServerState()
        end
        Print(text)
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

    local now = GetTime()
    if not M.allowed then
        -- No answer to HELLO yet: say so after a while, and keep asking.
        if not M.denied and now - M.helloAt >= 6 then
            if M.helloAt > 0 and not M.noAnswer then
                M.noAnswer = true
                UpdateServerState()
            end
            M.helloAt = now
            Send("HELLO")
        end
        return
    end

    if now - M.listAt >= LIST_EVERY then
        M.listAt = now
        Send("LIST")
    end
    -- Only while the bot is still online (in the list) and not reported gone.
    if M.selected and not M.gone and BotByGuid(M.selected) and OllamaMonitorDB and OllamaMonitorDB.auto ~= false then
        -- One page request at a time; a lost answer is retried after 10s.
        if (not M.waiting and now - M.pageAt >= PageEvery()) or (M.waiting and now - M.askedAt >= 10) then
            RequestPage()
        end
    end
end)

frame:SetScript("OnShow", function()
    if not M.allowed then
        M.helloAt = GetTime()
        Send("HELLO")
    else
        -- Ask again: the server's state may have changed while hidden.
        Send("HELLO")
    end
    M.listAt = 0
    autoCheck:SetChecked(not (OllamaMonitorDB and OllamaMonitorDB.auto == false))
    plainCheck:SetChecked(M.plain)
    UpdateTabs()
    UpdateHeader()
    UpdateCamera()
    RefreshList()
end)

-- ---------------------------------------------------------------------------
-- Log-only mode: only the bot's name and the page, small and see-through, so
-- the screen stays free while the camera follows a bot.
-- ---------------------------------------------------------------------------

-- Everything that is not the page.
local FULL_ONLY = { titleBar, side, card, actions, tabBar, footer }

frame:SetMinResize(300, 150)
frame:SetMaxResize(1600, 1200)

local grip = CreateFrame("Button", nil, frame)
grip:SetSize(16, 16)
grip:SetPoint("BOTTOMRIGHT", -2, 2)
grip:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")
grip:SetHighlightTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Highlight")
grip:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Down")
grip:SetScript("OnMouseDown", function() frame:StartSizing("BOTTOMRIGHT") end)
grip:SetScript("OnMouseUp", function()
    frame:StopMovingOrSizing()
    OllamaMonitorDB = OllamaMonitorDB or {}
    OllamaMonitorDB.compactSize = { frame:GetWidth(), frame:GetHeight() }
end)
grip:Hide()

-- In log-only mode the window itself is the drag handle.
frame:RegisterForDrag("LeftButton")
frame:SetScript("OnDragStart", function(self) if M.compact then self:StartMoving() end end)
frame:SetScript("OnDragStop", function(self) self:StopMovingOrSizing(); SavePosition() end)

local function SetCompact(on)
    M.compact = on
    OllamaMonitorDB = OllamaMonitorDB or {}
    OllamaMonitorDB.compact = on

    for _, region in ipairs(FULL_ONLY) do
        if on then region:Hide() else region:Show() end
    end

    textBg:ClearAllPoints()
    if on then
        local size = OllamaMonitorDB.compactSize or { 460, 300 }
        frame:SetSize(size[1], size[2])
        Flat(frame, { 0, 0, 0 }, 0.55, { 0.3, 0.3, 0.35 })
        textBg:SetPoint("TOPLEFT", frame, "TOPLEFT", 4, -24)
        textBg:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -4, 4)
        textBg:SetBackdropColor(0, 0, 0, 0)
        textBg:SetBackdropBorderColor(0, 0, 0, 0)
        sizeButton:SetNormalTexture("Interface\\Buttons\\UI-Panel-BiggerButton-Up")
        sizeButton:SetPushedTexture("Interface\\Buttons\\UI-Panel-BiggerButton-Down")
        compactHeader:Show()
        grip:Show()
    else
        frame:SetSize(FULL_W, FULL_H)
        Flat(frame, C.window, 0.97, C.line)
        textBg:SetPoint("TOPLEFT", tabBar, "BOTTOMLEFT", 0, -8)
        textBg:SetPoint("BOTTOMRIGHT", footer, "TOPRIGHT", 0, 8)
        Flat(textBg, C.content, 1, C.line)
        sizeButton:SetNormalTexture("Interface\\Buttons\\UI-Panel-SmallerButton-Up")
        sizeButton:SetPushedTexture("Interface\\Buttons\\UI-Panel-SmallerButton-Down")
        compactHeader:Hide()
        grip:Hide()
    end
    UpdateServerState()
end
M.SetCompact = SetCompact

sizeButton:SetScript("OnClick", function() SetCompact(not M.compact) end)
sizeButton:SetScript("OnEnter", function(self)
    GameTooltip:SetOwner(self, "ANCHOR_LEFT")
    GameTooltip:AddLine(M.compact and "Full window" or "Log only")
    GameTooltip:AddLine(M.compact and "Back to the full window." or "Shrink to the page alone, see-through, to watch while following a bot.",
        0.8, 0.8, 0.8, true)
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
    OllamaMonitorDB = OllamaMonitorDB or {}
    if arg == "stop" then
        Send("UNWATCH")
    elseif arg == "pause" then
        OllamaMonitorDB.auto = false
        autoCheck:SetChecked(false)
        Print("auto refresh paused (/om resume)")
    elseif arg == "resume" then
        OllamaMonitorDB.auto = true
        autoCheck:SetChecked(true)
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
