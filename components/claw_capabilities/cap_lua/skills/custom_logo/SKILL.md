---
{
  "name": "custom_logo",
  "description": "Generate a custom SVG logo for the device LCD based on the user's description, save it to storage, and display it on the device screen replacing the idle animation.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Custom Logo

Use this skill when the user asks to create, generate, design, or change the device logo or pet logo. Also use it when the user describes what their logo should look like ("make a cat logo", "I want a rocket ship icon", "换一个新 logo", "生成一个宠物 logo").

## Core Rules

1. Generate valid SVG markup based on the user's description.
2. The SVG should be simple (suitable for a small LCD display, typically 240-320 px wide).
3. Use basic SVG elements: `<rect>`, `<circle>`, `<ellipse>`, `<path>`, `<polygon>`, `<line>`, `<text>`.
4. Keep the SVG viewBox at `0 0 320 240` (landscape) or `0 0 240 240` (square) to match the LCD.
5. Use solid fills and simple gradients. Avoid complex filters, masks, or external references.
6. Run `generate_logo.lua` with the SVG content as `svg_content` arg.
7. If the user says "delete logo", "remove logo", or "restore default", tell them to use the web config UI Logo page or run the file manager to delete `/fatfs/logo/custom.svg`.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "svg_content": {
      "type": "string",
      "description": "Complete SVG markup string (must start with <svg and end with </svg>)."
    }
  },
  "required": ["svg_content"]
}
```

## Tool Call Inputs

Generate and apply a custom logo:

```json
{"path":"{CUR_SKILL_DIR}/scripts/generate_logo.lua","args":{"svg_content":"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 320 240\"><rect width=\"320\" height=\"240\" fill=\"#1a1a2e\"/><circle cx=\"160\" cy=\"120\" r=\"80\" fill=\"#e94560\"/><text x=\"160\" y=\"135\" text-anchor=\"middle\" font-size=\"32\" fill=\"white\">Claw</text></svg>"}}
```

## Recommended Flow

1. Understand the user's description (style, colours, subject).
2. Generate a complete SVG string that matches the description.
3. Run `{CUR_SKILL_DIR}/scripts/generate_logo.lua` with `svg_content` set to the generated SVG.
4. Report the saved path and remind the user the logo will automatically update on the device screen within a few seconds, and can also be previewed at the device's web config UI Logo page.
5. If the script returns an error, report that error directly.
