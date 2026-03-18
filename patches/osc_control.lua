-- patches/osc_control.lua
-- Manual OSC control of shader parameters with linear interpolation.
--
-- /vj/shader  i  <index>                   switch shader
-- /vj/p0 .. /vj/p14  f  <value>           set uniform immediately
-- /vj/animate  ifff  <param> <from> <to> <duration>  animate over time
--
-- Example:
--   oscsend localhost 9000 /vj/animate ifff 0 2.0 12.0 3.0
--   (animate u_p[0] from 2 to 12 over 3 seconds)

local params = {}
for i = 0, 14 do params[i] = 0.0 end

-- Active animations: params[i] = {from, to, duration, elapsed}
local anims = {}

function on_osc(addr, val)
    if addr == "/vj/shader" then
        vj.shader(math.floor(val))
    elseif addr == "/vj/p0"  then params[0]  = val
    elseif addr == "/vj/p1"  then params[1]  = val
    elseif addr == "/vj/p2"  then params[2]  = val
    elseif addr == "/vj/p3"  then params[3]  = val
    elseif addr == "/vj/p4"  then params[4]  = val
    elseif addr == "/vj/p5"  then params[5]  = val
    elseif addr == "/vj/p6"  then params[6]  = val
    elseif addr == "/vj/p7"  then params[7]  = val
    elseif addr == "/vj/p8"  then params[8]  = val
    elseif addr == "/vj/p9"  then params[9]  = val
    elseif addr == "/vj/p10" then params[10] = val
    elseif addr == "/vj/p11" then params[11] = val
    elseif addr == "/vj/p12" then params[12] = val
    elseif addr == "/vj/p13" then params[13] = val
    elseif addr == "/vj/p14" then params[14] = val
    elseif addr == "/vj/anim_clear" then on_osc_anim_clear(val)
    end
end

function on_animate(param, from, to, duration)
    anims[param] = { from=from, to=to, duration=duration, elapsed=0.0 }
end

-- /vj/anim_clear i <param>   clear animation for one param
-- /vj/anim_clear             (sent as float -1) clear all animations
function on_osc_anim_clear(val)
    local param = math.floor(val)
    if param < 0 then
        anims = {}
    else
        anims[param] = nil
    end
end

function on_frame(dt)
    -- Tick animations
    for i, a in pairs(anims) do
        a.elapsed = a.elapsed + dt
        local t = a.elapsed / a.duration
        if t >= 1.0 then
            t = 1.0
            params[i] = a.to
            anims[i] = nil
        else
            params[i] = a.from + (a.to - a.from) * t
        end
    end

    -- Apply all params
    for i = 0, 14 do
        vj.uniform(i, params[i])
    end
end
