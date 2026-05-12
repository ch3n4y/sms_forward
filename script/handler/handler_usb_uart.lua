module(..., package.seeall)

local UART_ID = 3
local RX_BUF = ""

local function uartWriteLine(payload, uart_id)
    local eol = (type(config.USB_NOTIFY_EOL) == "string" and config.USB_NOTIFY_EOL ~= "") and config.USB_NOTIFY_EOL or "\r\n"
    local target_uart_id = uart_id or UART_ID
    uart.write(target_uart_id, payload .. eol)
end

local function getPhoneIdentity()
    local number = sim.getNumber()
    if number and number ~= "" then
        return tostring(number):gsub("^86", ""), "phone_number"
    end

    local config_number = config.NUMBER
    if type(config_number) == "string" and config_number ~= "" then
        return config_number, "configured_number"
    end

    local iccid = sim.getIccid()
    if iccid and iccid ~= "" then
        return tostring(iccid), "iccid"
    end

    return "", "unknown"
end

local function reply(event, ok, extra, uart_id)
    local payload = {
        channel = "usb_uart",
        event = event,
        ok = ok and true or false,
        timestamp = util_status.formatTimestamp(),
    }
    if type(extra) == "table" then
        for k, v in pairs(extra) do
            payload[k] = v
        end
    end
    uartWriteLine(json.encode(payload), uart_id)
end

local function applyHostTime(data, uart_id)
    local clock = data.clock
    if type(clock) ~= "table" then
        reply("sync_time", false, { reason = "missing clock" }, uart_id)
        return
    end

    local t = {
        year = tonumber(clock.year),
        month = tonumber(clock.month),
        day = tonumber(clock.day),
        hour = tonumber(clock.hour),
        min = tonumber(clock.min),
        sec = tonumber(clock.sec),
    }

    if not t.year or not t.month or not t.day or not t.hour or not t.min or not t.sec then
        reply("sync_time", false, { reason = "invalid clock fields" }, uart_id)
        return
    end

    misc.setClock(t, function(now, result)
        reply("sync_time", result, {
            clock = now,
            source = data.source or "host",
        }, uart_id)
    end)
end

local function replyStatus(uart_id)
    local rsrp = net.getRsrp() - 140
    local signal = ""
    local temperature = util_temperature.get()
    if rsrp ~= 0 then
        signal = tostring(rsrp) .. "dBm"
    end
    if temperature == "-99" then
        temperature = ""
    else
        temperature = tostring(temperature) .. "℃"
    end

    local phone_identity, identity_type = getPhoneIdentity()

    reply("get_status", true, {
        phone_number = phone_identity,
        identity_type = identity_type,
        operator = util_mobile.getOper(true),
        signal_strength = signal,
        temperature = temperature,
        poweron_reason = tostring(rtos.poweron_reason()),
        poweron_reason_zh = util_status.bootPoweronReasonZh(),
    }, uart_id)
end

local function rebootDevice(uart_id)
    reply("reboot", true, { message = "rebooting" }, uart_id)
    sys.timerStart(rtos.restart, 500)
end

local function handleLine(line, uart_id)
    log.info("handler_usb_uart", "recv", uart_id, line)
    local ok, data = pcall(json.decode, line)
    if not ok or type(data) ~= "table" then
        log.warn("handler_usb_uart", "invalid json", uart_id, line)
        return
    end

    if data.cmd == "sync_time" then
        applyHostTime(data, uart_id)
    elseif data.cmd == "get_status" then
        replyStatus(uart_id)
    elseif data.cmd == "reboot" then
        rebootDevice(uart_id)
    else
        log.warn("handler_usb_uart", "unknown cmd", uart_id, data.cmd)
    end
end

local function onReceive(uart_id)
    local current_uart_id = uart_id or UART_ID
    while true do
        local chunk = uart.read(current_uart_id, "*l")
        if not chunk or #chunk == 0 then
            break
        end
        RX_BUF = RX_BUF .. chunk
        local line = RX_BUF:gsub("[\r\n]+$", "")
        RX_BUF = ""
        if line ~= "" then
            handleLine(line, current_uart_id)
        end
    end
end

local function setupManagedUarts()
    local ok, err = pcall(uart.setup, UART_ID, 115200, 8, uart.PAR_NONE, uart.STOP_1)
    if ok then
        log.info("handler_usb_uart", "uart setup ok", UART_ID)
    else
        log.error("handler_usb_uart", "uart setup failed", UART_ID, err)
    end
    uart.on(UART_ID, "receive", function()
        onReceive(UART_ID)
    end)
    log.info("handler_usb_uart", "receive callback attached", UART_ID)
end

setupManagedUarts()
