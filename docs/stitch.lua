#!/usr/bin/env lua

local function die(msg)
  io.stderr:write("stitch: ", msg, "\n")
  os.exit(1)
end

-- Default settings
local tolerant = false
local prefix = "stitch"   -- default directive prefix

-- Parse flags
local i = 1
while i <= #arg do
  local a = arg[i]

  if a == "--tolerant" or a == "-t" then
    tolerant = true
    table.remove(arg, i)

  elseif a == "--prefix" or a == "-p" then
    table.remove(arg, i)        -- remove flag
    prefix = arg[i]             -- next argument is the value
    if not prefix then
      die("missing value for --prefix")
    end
    table.remove(arg, i)        -- remove value

  else
    -- first non-flag is the input file
    break
  end
end

-- Input file or stdin
local infile = arg[1]
local input
if infile then
  input = io.open(infile, "r")
  if not input then
    die("cannot open input file: " .. infile)
  end
else
  input = io.stdin
end

local function read_all(f)
  local content = f:read("*a")
  if f ~= io.stdin then f:close() end
  return content or ""
end

local function resolve_path(base, target)
  if target:match("^/") or not base or base == "" then
    return target
  end
  if not base:match("/$") then
    base = base .. "/"
  end
  return base .. target
end

-- Recursive expansion helper
local run_stage1 -- forward declare

local function expand_include(title, target, spec, base, seen)
  seen = seen or {}
  local resolved = resolve_path(base, target)

  if seen[resolved] then
    die("recursive include detected: " .. resolved)
  end

  local f = io.open(resolved, "r")
  if not f then
    if tolerant then
      -- Drop the span, keep the link
      return "[" .. title .. "](" .. target .. ")"
    else
      die("cannot open included file: " .. resolved)
    end
  end

  seen[resolved] = true
  local content = f:read("*a") or ""
  f:close()

  local child_base = resolved:match("(.*/)")
  content = run_stage1(content, child_base, seen)
  seen[resolved] = nil

  return content
end


-- Directive table (keyed by class name without prefix)
local directives = {
  ["include"] = {
    stage = 1,
    recursive = true,
    pattern = function(class)
      -- <span class="stitch-include"/>[Title](path)
      return '<span class="' .. class .. '"%s*/>%s*%[([^%]]+)%]%(([^)]+)%)'
    end,
    replace = function(spec, title, target)
      return expand_include(title, target, spec, spec._base, spec._seen)
    end,
  },

  ["replace"] = {
    stage = 1,
    recursive = true,
    pattern = function(class)
      -- <span class="stitch-replace"/>[Title](path)
      return '[^\n]*<span class="' .. class .. '"%s*/>%s*%[([^%]]+)%]%(([^)]+)%)[^\n]*\n'
    end,
    replace = function(spec, title, target)
      return expand_include(title, target, spec, spec._base, spec._seen)
    end,
  },

  ["trim-line"] = {
    stage = 2,
    pattern = function(class)
      -- Remove entire line containing the directive
      return '[^\n]*<span class="' .. class .. '"%s*/>[^\n]*\n'
    end,
    replace = "",
  },

  ["trim-before"] = {
    stage = 2,
    pattern = function(class)
      -- Remove everything before the directive on the same line
      return '[^\n]*<span class="' .. class .. '"%s*/>'
    end,
    replace = "",
  },

  ["trim-after"] = {
    stage = 2,
    pattern = function(class)
      -- Remove everything after the directive on the same line
      return '<span class="' .. class .. '"%s*/>[^ \n]*'
    end,
    replace = "",
  },
}

-- Compile directives: apply prefix, escape, build patterns
for class, spec in pairs(directives) do
  local full = prefix .. "-" .. class
  local escaped = full:gsub("%-", "%%-")

  spec.full = full
  spec.class_escaped = escaped
  spec.pattern = spec.pattern(escaped)
end

-- Build stages
local stages = {}
for _, spec in pairs(directives) do
  stages[spec.stage] = stages[spec.stage] or {}
  table.insert(stages[spec.stage], spec)
end

run_stage1 = function(text, base, seen)
  seen = seen or {}

  for _, spec in ipairs(stages[1] or {}) do
    spec._base = base
    spec._seen = seen

    text = text:gsub(spec.pattern, function(...)
      local out = spec.replace(spec, ...)

      -- Only recurse if the directive opts in
      if spec.recursive and type(out) == "string" then
        local child_base = spec._base
        return run_stage1(out, child_base, seen)
      end

      return out
    end)
  end

  return text
end

-- Main
local doc = read_all(input)
local base = infile and infile:match("(.*/)") or nil

doc = run_stage1(doc, base, {})

-- Later stages: simple pattern/replace passes
for stage = 2, #stages do
  local specs = stages[stage]
  if specs then
    for _, spec in ipairs(specs) do
      if type(spec.replace) == "string" then
        doc = doc:gsub(spec.pattern, spec.replace)
      else
        doc = doc:gsub(spec.pattern, function(...)
          return spec.replace(spec, ...)
        end)
      end
    end
  end
end

io.write(doc)
