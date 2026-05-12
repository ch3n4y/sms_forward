PROJECT = "air724ug-forwarder"
VERSION = "1.0.0"

require "log"
LOG_LEVEL = log.LOGLEVEL_INFO
require "config"
require "audio"
audio.setStrategy(1)
require "cc"
require "common"
require "http"
require "misc"
require "net"
require "netLed"
require "ntp"
require "powerKey"
require "record"
require "ril"
require "sim"
require "sms"
require "sys"
require "util_mobile"
require "util_audio"
require "util_http"
require "util_notify"
require "util_temperature"
require "util_ntp"
require "util_status"
require "handler_call"
require "handler_powerkey"
require "handler_sms"
require "handler_usb_uart"
require "usbmsc"

-- 输出音频通道选项, 0:听筒 1:耳机 2:喇叭
-- 输入音频通道选项, 0:main_mic 1:auxiliary_mic 3:headphone_mic_left 4:headphone_mic_right

-- 静音音频通道
AUDIO_OUTPUT_CHANNEL_MUTE = 0
AUDIO_INPUT_CHANNEL_MUTE = 1
-- 正常音频通道
AUDIO_OUTPUT_CHANNEL_NORMAL = 2
AUDIO_INPUT_CHANNEL_NORMAL = 0

audio.setChannel(AUDIO_OUTPUT_CHANNEL_NORMAL, AUDIO_INPUT_CHANNEL_NORMAL)

-- 配置内部 PA 类型 audiocore.CLASS_AB, audiocore.CLASS_D
audiocore.setpa(audiocore.CLASS_D)
-- 配置外部 PA
-- pins.setup(pio.P0_14, 0)
-- audiocore.pa(pio.P0_14, 1, 0, 0)
-- audio.setChannel(1)

-- 设置睡眠等待时间
-- ril.request("AT+WAKETIM=0")

-- 定时查询温度
sys.timerLoopStart(util_temperature.get, 1000 * 60)
-- 定时查询 信号强度 基站信息
net.startQueryAll(1000 * 60, 1000 * 60 * 10)

local function hasNetworkNotifyChannel()
    local notify_type = config.NOTIFY_TYPE
    if type(notify_type) ~= "table" then
        notify_type = { notify_type }
    end

    for _, channel in ipairs(notify_type) do
        if type(channel) == "string" and channel ~= "" and channel ~= "usb_uart" then
            return true
        end
    end

    return false
end

local function needNetworkReady()
    if config.RNDIS_ENABLE then
        return true
    end
    if type(config.UPLOAD_URL) == "string" and config.UPLOAD_URL ~= "" then
        return true
    end
    if config.NTP_AUTO_SYNC ~= false then
        return true
    end
    if hasNetworkNotifyChannel() then
        return true
    end
    return false
end

sys.taskInit(function()
    -- 等待 U 盘挂载
    sys.waitUntil("USBMSC_MOUNTED", 2000)

    -- 尝试读取 U 盘配置文件
    -- 优先级: 明文 > 密文
    -- local config_path_plain = "/usbmsc0/config.lua"
    local config_path_cipher = "/usbmsc0/config.bin"

    -- if io.exists(config_path_plain) then
    --     log.info("main", "load config from " .. config_path_plain)
    --     local chunk, err = loadfile(config_path_plain)
    --     if chunk then
    --         chunk("config")
    --     else
    --         log.error("main", "load config error", err)
    --     end
    if io.exists(config_path_cipher) then
        log.info("main", "load config from " .. config_path_cipher)
        local f = io.open(config_path_cipher, "rb")
        if f then
            local cipher_content = f:read("*a")
            f:close()

            local key = "thehot"

            local _key = crypto.md5(key, #key)

            local decoded_content = crypto.base64_decode(cipher_content, #cipher_content)
            -- 截取 _key 0-16 位 小写
            local plain_content = crypto.aes_decrypt("ECB", "ZERO", decoded_content, _key:sub(1, 16):lower())

            if plain_content then
                log.info("main", "config decrypted success")
                local chunk, err = loadstring(plain_content)
                if chunk then
                    local result, run_err = pcall(chunk, "config")
                    if not result then
                        log.error("main", "config apply error", run_err)
                    else
                        log.info("main", "config apply success")
                    end
                else
                    log.error("main", "loadstring error", err)
                end
            else
                log.error("main", "decrypt failed")
            end
        end
    else
        log.info("main", "load config from default config.lua")
    end

    -- RNDIS
    ril.request("AT+RNDISCALL=" .. (config.RNDIS_ENABLE and 1 or 0) .. ",0")

    -- NET 指示灯, LTE 指示灯
    if config.LED_ENABLE then
        pmd.ldoset(2, pmd.LDO_VLCD)
    end
    netLed.setup(true, pio.P0_1, pio.P0_4)
    netLed.updateBlinkTime("SCK", 50, 50)
    netLed.updateBlinkTime("GPRS", 200, 2000)

    -- 开机查询本机号码
    sim.setQueryNumber(true)
    ril.request("AT+CNUM")
    sys.timerStart(ril.request, 3000, "AT+CNUM")
    -- 如果查询不到本机号码, 可以取消下面注释的代码, 尝试手动写入到 SIM 卡, 写入成功后注释掉即可
    -- sys.timerStart(ril.request, 5000, 'AT+CPBS="ON"')
    -- sys.timerStart(ril.request, 6000, 'AT+CPBW=1,"+8618888888888",145')

    -- SIM 自动切换开关
    ril.request("AT*SIMAUTO=1")

    -- 等待网络就绪
    if needNetworkReady() then
        sys.waitUntil("IP_READY_IND", 1000 * 60 * 2)
    else
        log.info("main", "skip wait IP_READY_IND in offline usb mode")
    end

    -- 等待获取 Band 值
    -- sys.wait(1000 * 5)

    -- 开机通知
    if config.BOOT_NOTIFY then
        local function boot_notify_prefix()
            local p = config.BOOT_NOTIFY_PREFIX
            if type(p) ~= "string" then p = "phone_imsi" end
            local mode = (p == "phone_imsi" or p == "imsi_tail") and p or "fixed"
            if mode == "phone_imsi" or mode == "imsi_tail" then
                local n = tonumber(config.BOOT_NOTIFY_PREFIX_TAIL_LEN) or tonumber(config.BOOT_NOTIFY_IMSI_TAIL_LEN) or 6
                if n < 1 then n = 6 end
                local function digits_id(s)
                    if not s or s == "" then return "" end
                    s = tostring(s):gsub("%D", "")
                    s = s:gsub("^86+", "")
                    return s
                end
                local function tail_from(id)
                    if id == "" then return "" end
                    return string.sub(id, -n)
                end
                local id = ""
                if mode == "phone_imsi" then
                    local function phone_from_sim()
                        local x = digits_id(sim.getNumber())
                        if x ~= "" then return x end
                        for _ = 1, 8 do
                            ril.request("AT+CNUM")
                            sys.wait(800)
                            x = digits_id(sim.getNumber())
                            if x ~= "" then return x end
                        end
                        return ""
                    end
                    id = phone_from_sim()
                    if id == "" then
                        local cn = config.NUMBER
                        if type(cn) == "string" and cn ~= "" then
                            id = digits_id(cn)
                        end
                    end
                end
                if id == "" then
                    local imsi = sim.getImsi()
                    if imsi == nil or imsi == "" then
                        sys.waitUntil("IMSI_READY", 8000)
                        imsi = sim.getImsi()
                    end
                    id = imsi and tostring(imsi):gsub("%s+", "") or ""
                end
                local tail = tail_from(id)
                if tail == "" then
                    log.warn("main", "boot notify: no phone/imsi, prefix empty")
                    return ""
                end
                return "#" .. tail .. "_"
            end
            return p
        end
        local function boot_notify_suffix()
            local s = config.BOOT_NOTIFY_SUFFIX
            if type(s) ~= "string" then s = "reason_zh" end
            if s == "none" then return "" end
            if s == "reason" then return tostring(rtos.poweron_reason()) end
            if s == "reason_zh" then return util_status.bootPoweronReasonZh() end
            return s
        end
        local prefix = boot_notify_prefix()
        local suffix = boot_notify_suffix()
        local pm = config.BOOT_NOTIFY_PREFIX
        if type(pm) ~= "string" then pm = "" end
        if suffix == "" and (pm == "phone_imsi" or pm == "imsi_tail") and string.sub(prefix, -1) == "_" then
            prefix = string.sub(prefix, 1, -2)
        end
        util_notify.add(prefix .. suffix)
    end

    -- 定时查询流量
    if config.QUERY_TRAFFIC_INTERVAL and config.QUERY_TRAFFIC_INTERVAL >= 1000 * 60 then
        sys.timerLoopStart(util_mobile.queryTraffic, config.QUERY_TRAFFIC_INTERVAL)
    end

    -- 开机同步时间
    if config.NTP_AUTO_SYNC ~= false then
        util_ntp.sync()
        sys.timerLoopStart(util_ntp.sync, 1000 * 30)
    end
end)

-- 验证 PIN 码
sys.subscribe("SIM_IND", function(msg)
    if msg == "SIM_PIN" then
        util_mobile.pinVerify()
    end
end)

-- 系统初始化
sys.init(0, 0)
sys.run()
