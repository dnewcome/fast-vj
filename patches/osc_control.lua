-- patches/osc_control.lua
-- Manual OSC control of shader parameters. No audio reactivity.
--
-- Send OSC to control:
--   /vj/shader  i <index>   -- switch shader
--   /vj/p       i f <index> <value>  -- set u_p[index]
--   /vj/p0 .. /vj/p14  f <value>    -- set individual uniform

local params = {}
for i = 0, 14 do params[i] = 0.0 end

function on_osc(addr, val)
    if addr == "/vj/shader" then
        vj.shader(math.floor(val))
    elseif addr == "/vj/p0"  then params[0]  = val; vj.uniform(0,  val)
    elseif addr == "/vj/p1"  then params[1]  = val; vj.uniform(1,  val)
    elseif addr == "/vj/p2"  then params[2]  = val; vj.uniform(2,  val)
    elseif addr == "/vj/p3"  then params[3]  = val; vj.uniform(3,  val)
    elseif addr == "/vj/p4"  then params[4]  = val; vj.uniform(4,  val)
    elseif addr == "/vj/p5"  then params[5]  = val; vj.uniform(5,  val)
    elseif addr == "/vj/p6"  then params[6]  = val; vj.uniform(6,  val)
    elseif addr == "/vj/p7"  then params[7]  = val; vj.uniform(7,  val)
    elseif addr == "/vj/p8"  then params[8]  = val; vj.uniform(8,  val)
    elseif addr == "/vj/p9"  then params[9]  = val; vj.uniform(9,  val)
    elseif addr == "/vj/p10" then params[10] = val; vj.uniform(10, val)
    elseif addr == "/vj/p11" then params[11] = val; vj.uniform(11, val)
    elseif addr == "/vj/p12" then params[12] = val; vj.uniform(12, val)
    elseif addr == "/vj/p13" then params[13] = val; vj.uniform(13, val)
    elseif addr == "/vj/p14" then params[14] = val; vj.uniform(14, val)
    end
end

function on_frame(dt)
    -- Re-apply params each frame so they persist across shader switches
    for i = 0, 14 do
        vj.uniform(i, params[i])
    end
end
