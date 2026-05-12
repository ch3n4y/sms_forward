module(..., package.seeall)

function bootPoweronReasonZh()
    local r = rtos.poweron_reason()
    local tab = {
        [0] = "电源键或上电开机",
        [1] = "充电或下载完成开机",
        [2] = "闹钟开机",
        [3] = "软件重启",
        [4] = "原因未知",
        [5] = "RESET键复位",
        [6] = "异常重启",
        [7] = "工具控制重启",
        [8] = "内部看门狗重启",
        [9] = "外部复位",
        [10] = "充电开机",
    }
    local name = tab[r]
    if name then return name end
    if rtos.POWERON_CHARGER and r == rtos.POWERON_CHARGER then return "充电开机" end
    return "开机原因(" .. tostring(r) .. ")"
end

function formatTimestamp()
    if misc and misc.getClock then
        local ok, clk = pcall(misc.getClock)
        if ok and type(clk) == "table" then
            local year = tonumber(clk.year)
            local month = tonumber(clk.month)
            local day = tonumber(clk.day)
            local hour = tonumber(clk.hour)
            local min = tonumber(clk.min)
            local sec = tonumber(clk.sec)
            if year and month and day and hour and min and sec and year >= 2024 then
                return string.format("%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, min, sec)
            end
        end
    end
    return ""
end
