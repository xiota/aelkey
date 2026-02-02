local M = ...

local function noop(...) return nil end

local proxy = {}
setmetatable(proxy, {
  __call = function() return noop end,   -- allow calls
  __index = function() return proxy end, -- any field returns proxy again
  __newindex = function() end,
  __metatable = false
})

local env = {
  require = function(name)
    return proxy
  end
}

setmetatable(env, {
  __index = function(_, k)
    return proxy   -- any unknown global resolves to proxy
  end,
  __newindex = function(_, k, v)
    rawset(env, k, v)  -- allow script to define tables
  end,
  __metatable = false
})

function M.inspect_file(path)
  local chunk, err = loadfile(path, "t", env)
  if not chunk then return nil, err end
  local ok, res = pcall(chunk)
  if not ok then return nil, res end
  return env.inputs, env.outputs
end

function M.inspect_string(code)
  local chunk, err = load(code, "t", env)
  if not chunk then return nil, err end
  local ok, res = pcall(chunk)
  if not ok then return nil, res end
  return env.inputs, env.outputs
end
