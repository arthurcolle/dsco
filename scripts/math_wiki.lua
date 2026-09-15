-- Give every display equation a stable, concept-scoped HTML resource.

local concept_slug = "preamble"
local local_equation = 0
local global_equation = 0

local function slugify(value)
  local slug = pandoc.utils.stringify(value):lower()
  slug = slug:gsub("[^a-z0-9%s%-]", "")
  slug = slug:gsub("[%s%-]+", "-")
  slug = slug:gsub("^%-+", ""):gsub("%-+$", "")
  return slug
end

local function is_display_math_paragraph(block)
  if block.t ~= "Para" and block.t ~= "Plain" then
    return false
  end
  if #block.content ~= 1 then
    return false
  end
  local inline = block.content[1]
  return inline.t == "Math" and inline.mathtype == "DisplayMath"
end

function Pandoc(document)
  local output = pandoc.List()

  for _, block in ipairs(document.blocks) do
    if block.t == "Header" and block.level == 3 then
      concept_slug = slugify(block.content)
      local_equation = 0
    end

    if is_display_math_paragraph(block) then
      local_equation = local_equation + 1
      global_equation = global_equation + 1

      local equation_id =
        "eq-" .. concept_slug .. "-" .. tostring(local_equation)
      local attributes = {
        { "data-resource", "equation" },
        { "data-concept", "#wiki-" .. concept_slug },
        { "data-equation-index", tostring(global_equation) },
        {
          "aria-label",
          "Equation " .. tostring(local_equation) .. " in " .. concept_slug
        },
        { "role", "group" }
      }

      output:insert(
        pandoc.Div(
          { block },
          pandoc.Attr(equation_id, { "equation-card" }, attributes)
        )
      )
    else
      output:insert(block)
    end
  end

  document.blocks = output
  return document
end
