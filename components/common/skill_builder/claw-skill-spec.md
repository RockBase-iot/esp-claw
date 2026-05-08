# Claw Skill Component Spec

This document defines the standard `skills/` directory format, build sync rules, and filesystem path conventions for component-provided skills.

## Directory Layout

Any build component may provide a `skills/` directory:

```text
component_xx/
└── skills/
    └── skill_id/
        ├── SKILL.md
        ├── references/
        │   └── guide.md
        ├── scripts/
        │   └── action.lua
        └── assets/
            └── image.bin
```

Notes:

- `skills/<skill_id>/` is one complete skill.
- `SKILL.md` is the only required file.
- `references/`, `scripts/`, `assets/`, and other subdirectories are optional.
- The whole skill directory is copied unchanged into `skills/<skill_id>/` in the application file image.

## SKILL.md Rules

`SKILL.md` must start with JSON frontmatter:

```md
---
{
  "name": "skill_id",
  "description": "Short capability description.",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  }
}
---

# Skill Title
```

Rules:

- The frontmatter must be wrapped by `---`.
- The frontmatter must be a valid JSON object.
- `name` must be a non-empty string.
- `name` must match the parent directory name `skills/<skill_id>`.
- `description` should briefly describe when to use the skill.
- `metadata.cap_groups` declares the capability groups that need to be activated.
- `metadata.manage_mode` should currently be `readonly`.

### Description

`description` affects skill matching, so it must describe user intent rather than implementation details.

Rules:

- Include common user wording, such as turn on/off, set color, brightness, LED strip/light.
- Include critical prerequisites, such as `Requires board_hardware_info skill for the GPIO.`.
- Keep it to one sentence and avoid long paragraphs.
- Do not describe only internal script names, module names, or generic phrases such as `Run Lua script`.

Example:

```json
"description": "Turn the board LED strip/light on or off, set color or brightness. Requires board_hardware_info skill."
```

## Build Sync Rules

During build, `sync_component_skills.py` scans the `skills/` directory of every build component.

Sync rules:

- Every `skills/<skill_id>/SKILL.md` must exist.
- Every skill id must be unique across the whole project.
- Every file in the skill directory is copied into `skills/<skill_id>/` in the FATFS image.
- `references/`, `scripts/`, `assets/`, and other subdirectories keep their relative paths.
- The build fails if two components provide the same `skills/<skill_id>/...` output path.
- Old component skill files recorded in the build manifest are removed from the output directory when they no longer exist.

Source example:

```text
component_xx/skills/light_switch/SKILL.md
component_xx/skills/light_switch/scripts/switch.lua
```

Copied output:

```text
skills/light_switch/SKILL.md
skills/light_switch/scripts/switch.lua
```

The `skills/...` paths above are relative paths in the device filesystem or FATFS image, not source-tree paths.

## `{CUR_SKILL_DIR}` Placeholder

When `SKILL.md` is loaded, the skill runtime replaces `{CUR_SKILL_DIR}` in the document body with the current skill filesystem directory.

Example:

```md
Run `{CUR_SKILL_DIR}/scripts/action.lua` with `lua_run_script`.
```

Rules:

- `{CUR_SKILL_DIR}` is expanded only in the `SKILL.md` body, not in JSON frontmatter.
- The expanded value points to the current skill directory in the device filesystem or FATFS image.
- Use `{CUR_SKILL_DIR}/scripts/...` when passing a script path from this skill to a tool.

## Filesystem Path Rules

When the model reads files bundled with a skill, use filesystem-relative paths:

- `read_file("{CUR_SKILL_DIR}/references/guide.md")`
- `read_file("{CUR_SKILL_DIR}/scripts/action.lua")`
- `read_file("{CUR_SKILL_DIR}/assets/name.ext")`

Do not assume the source component directory name, and do not use `../` to move to a parent directory.

## Lua Skill Script Rules

If a skill contains Lua scripts, place them under `scripts/`:

```text
skills/<skill_id>/scripts/action.lua
```

Rules:

- Skill-local Lua scripts are read-only scripts distributed with the skill.
- User-facing actions should be implemented as standalone skills rather than `test/` entries.
- If script execution fails, the skill should require the model to report the error directly and avoid retrying with changed arguments.

## Naming And Conflict Rules

- Skill ids must be stable. Prefer lowercase letters, digits, underscores, or hyphens.
- Skill ids must exactly match their directory names.
- The project must not contain duplicate skill ids.
- The project must not contain duplicate skill output file paths.
- Do not expose the same user-facing action through multiple skills or script indexes.
