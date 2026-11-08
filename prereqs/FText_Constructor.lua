function Register()
    return "eb 12 48 8d ?? 24 ?? e8 ?? ?? ?? ?? ?? 02 00 00 00 48 8b 10"
end

function OnMatchFound(MatchAddress)
    local CallInstr = MatchAddress + 7
    local InstrSize = 5
    local NextInstr = CallInstr + InstrSize
    local Offset = DerefToInt32(CallInstr + 1)
    local ToStringAddress = NextInstr + Offset
    return ToStringAddress
end