# Contributing to Prestige Viewer

Thanks for your interest in Prestige.

## Reporting bugs

Open an issue in this repository's **Issues** tab and include:

- the version from **Help → About** (use the copy button)
- your Windows version and graphics card
- steps to reproduce the problem
- your log file, `%APPDATA%\PrestigeViewer\logs\Prestige.log` (remove anything private first)

If the same problem also happens in the official Second Life viewer, please report it to Linden Lab at [feedback.secondlife.com](https://feedback.secondlife.com/bug-reports) too.

## Code contributions

- Open or comment on an issue first, so the approach can be agreed before you write code.
- Rendering work targets the Vulkan renderer (`indra/newview/vkgfx`, shaders in `indra/vkclean/shaders`). New render paths use the persistent GPU scene, bindless materials, compute culling and indirect draws, not per-object binds or draws.
- Mark Prestige-specific changes to shared code with `<Prestige>` comments.
- Contributions are accepted under the project's license, the GNU LGPL 2.1.
