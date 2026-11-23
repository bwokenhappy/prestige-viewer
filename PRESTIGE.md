# Prestige Viewer (Slipstream engine)

Prestige Viewer is a Second Life viewer built on **Slipstream**, a GPU-driven **Vulkan** renderer. It has Prestige's own name, P logo and charcoal-and-orange look, and starts new users in Firestorm-style settings.

Prestige is a third-party viewer. It is not affiliated with Linden Lab, the Slipstream project (Bluefast Studios), The Phoenix Firestorm Project, or Black Dragon.

Base: Slipstream `main` as of 22 Nov 2026 (`Slipstream-main.zip`, version 1.7.0), committed as the first commit of this repository. Prestige changes are on the `prestige` branch and marked `<Prestige>` in code.

---

## What Prestige changes

### Name and identity
| Area | Change |
|---|---|
| App name | "Prestige" everywhere in the UI (12 languages), the window class, logs (`Prestige.log`), notifications, VR/OpenXR app name, Instant Replay videos (`Videos\Prestige`). |
| Settings folder | `%APPDATA%\PrestigeViewer` (Linux `~/.prestigeviewer`), so it never touches Slipstream's settings. **On first launch Prestige copies an existing Slipstream setup** (settings, logins, chat logs, presets). It copies and never moves, so Slipstream keeps working. |
| Channel / files | Channel `Prestige Release / Beta / Test`. Downloads are `Prestige_<version>_win64.zip` and `Prestige_<version>_Setup.exe`, and the program is `PrestigeViewer.exe`. |
| Installer | Name, shortcuts, publisher and uninstall entry say Prestige. **The optional "delete my data" uninstall step only removes `PrestigeViewer` folders, never Slipstream's.** |
| Logo | Your P on the app icon (Windows .ico used for the exe, installer and uninstaller), macOS .icns, the Linux icon theme, the About box logo and the transparent "mark" images. Small sizes use a solid-orange P so it stays readable. |
| About box | Introduces Prestige and keeps full credit to Slipstream / Bluefast Studios, Alchemy, Firestorm, Black Dragon and Linden Lab, as the LGPL requires. |

### Privacy and independence from Slipstream's servers
| Feature | In Slipstream | In Prestige |
|---|---|---|
| Update checker / auto-install | checks `slipstreamviewer.org` daily | **off**, no feed configured, settings hidden (otherwise it would replace Prestige with Slipstream) |
| Usage statistics (telemetry) | asks to send to `slipstreamviewer.org/api/telemetry` | **off**, no endpoint, no opt-in prompt. The Privacy tab says nothing is collected. |
| Discord link, website link | shown | hidden |
| Help wiki | Slipstream wiki | kept as "Slipstream Wiki (feature guide)", because it documents the same engine and features |
| Speech-to-text models | downloaded from Slipstream's server when that feature is turned on | unchanged (see release checklist) |

**Compatibility with Slipstream users:** pose sharing, precise positions, look-at, body tracking and the combat kill feed use Slipstream's in-world helper object (`#Slipstream Helper`). Prestige keeps the same protocol, so these features work between Prestige and Slipstream users. The UI says "Prestige/Slipstream users".

### Look
* New default skin **Prestige** (`skins/prestige`, picked from Preferences → Skins), made from the Slipstream skin.
  * Window body `#1F2023`, inputs `#17181B`, borders `#3A3B3F`, primary accent **`#F28C28`**, secondary accent `#F9A857`, and a warm-brown selection colour as in the design mock-up.
  * 178 skin images recoloured: selected/on/pressed states and tinted icons to Prestige orange, backgrounds to neutral charcoal, red warning icons unchanged.
* **Login splash**: `skins/default/html/en-us/prestige/login.html`, a local, offline page. It's shown on Second Life grids. OpenSim grids keep their own welcome pages, `ForceLoginURL` still overrides it, and the setting `PrestigeLocalLoginSplash` turns it off.

### Firestorm layout
* The welcome window **preselects "Firestorm"**, Slipstream's built-in Firestorm setup: `/` starts chat, right-click opens the pie menu, plain-text chat history, group chats wait in Conversations, 30-second notices, push-to-talk toggle, and other habits. Users confirm with Continue, or pick another viewer's habits.
* Already in Slipstream, ported from Firestorm: **People Nearby radar, Contacts window with contact sets, Area Search, Teleport History, Poser, Group Titles, Quick Preferences, legacy profiles, pie menus, LSL preprocessor**.
* **Not yet in Prestige** (Firestorm windows with no Slipstream equivalent), roughly in order of how much a Firestorm user would notice:
  1. Firestorm's **bottom login bar** (`panel_fs_login`), the layout in the design mock-up.
  2. Firestorm's **tabbed Conversations / nearby-chat window** (`floater_fs_im_container`, `floater_fs_nearby_chat`).
  3. Firestorm's **Preferences organisation** (its "Firestorm" tabs and option placement).
  4. Phototools, stream-title display and history, money tracker, wearable favourites, protected folders, pose stand, local asset blacklist, and the legacy search panels.

  Each needs its Firestorm C++ ported along with the layout file. Item 1 is the best next step.

### Graphics
Graphics come from Slipstream unchanged:
* Vulkan 1.3 with GPU culling and multi-draw indirect.
* Ray-traced shadows on RTX 20-series / RX 6000 and newer.
* PCSS soft shadows, GTAO, SSR, bloom, seven tone mappers, FSR/DLSS/TAA, and Photo Studio.

The OpenGL-only Black Dragon volumetric-lighting port from the Firestorm-based Prestige doesn't apply to this engine.

**Hardware:** a Vulkan 1.3 driver is required (NVIDIA GTX 750 / 900 series or newer, AMD RX 400 or newer, Intel HD/UHD 500/600 or newer). Older GPUs can't run Prestige on this engine.

---

## Building on Windows (WSL2)

Slipstream, and so Prestige, is built on Linux. The Windows version is cross-compiled. On a Windows PC, use **WSL2**. Allow about **30 GB** of disk; the first build downloads about 1 GB of libraries from Slipstream's server.

1. **Install WSL2 with Arch Linux.** Slipstream's maintained package list is for Arch. In PowerShell:
   ```powershell
   wsl --install archlinux
   ```
   (Ubuntu also works, but you'd need to work out the package names yourself; see Slipstream's wiki building page.)
2. **Install the build tools** inside Arch (Slipstream's list, plus the Windows cross-compile tools):
   ```sh
   sudo pacman -Syu base-devel cmake ninja git python pkgconf libpulse libunwind mesa glu \
     libxinerama libxcursor libxfixes libxft libpipewire dbus vlc gstreamer gst-plugins-base gst-plugins-good \
     vulkan-headers vulkan-icd-loader libdecor shaderc glslang clang lld llvm 7zip
   ```
   Then install **xwin** (the Microsoft CRT/SDK downloader) and unpack the Windows SDK:
   ```sh
   xwin --accept-license splat --output ~/.xwin
   ```
   For the `Setup.exe` you also need `makensis` (NSIS). Without it you still get the zip.
3. **Get the source** (see *Getting the source* below) into your Linux home folder, not `/mnt/c`, because building on the Windows drive is very slow.
4. **Build:**
   ```sh
   make                      # Linux build: a good first test that everything works
   make windows VIEWER_CHANNEL="Prestige Release"
   ```
   Windows output is in `build-windows-x86_64/package/`: `Prestige_<version>_win64.zip` (unzip and run `PrestigeViewer.exe`) and `Prestige_<version>_Setup.exe`.
   Expect 30–60 minutes on 4–8 CPU cores.

Linux note: the Linux binary and launcher keep their upstream names (`slipstream-bin`, `packaged/slipstream`) so Slipstream's Linux packaging scripts keep working. The deb/AUR/Flatpak scripts still say Slipstream and need rebranding before any Linux release.

### Getting the source (inside WSL)
You need `Slipstream-main.zip` (the snapshot Prestige is based on) and the `prestige-slipstream-patches` folder. On Windows, copy both somewhere like `C:\Users\<you>\Downloads`; WSL sees that as `/mnt/c/Users/<you>/Downloads`.
```sh
cd ~
unzip /mnt/c/Users/<you>/Downloads/Slipstream-main.zip      # creates ~/slipstream
mv slipstream prestige-viewer && cd prestige-viewer
git init -b main && git add -A && git commit -m "Slipstream main snapshot"
git checkout -b prestige
git am --keep-cr --whitespace=nowarn /mnt/c/Users/<you>/Downloads/prestige-slipstream-patches/*.patch
```
To publish it, create an empty repository (for example on GitHub) and push both branches.

### First-run test checklist
1. The login screen shows the Prestige splash, the P icon in the taskbar, and the charcoal-and-orange skin.
2. The welcome window opens with **Firestorm** highlighted.
3. If you use Slipstream, your settings and saved logins appear in Prestige, and Slipstream still works afterwards.
4. Help → About shows Prestige text, the P logo and full credits.
5. Preferences → Setup shows no update section. Preferences → Privacy → Usage statistics says nothing is collected, and no telemetry prompt appears.
6. In-world with a Slipstream user: pose sharing still works.
7. Uninstall with "delete my data" ticked, then confirm `%APPDATA%\SlipstreamViewer` is untouched.

---

## Before a public release

1. **Placeholder links.** About, error messages, the installer and the skin manifest point to `https://github.com/prestige-viewer/prestige-viewer` (plus `/releases` and `/issues`). Create that repository or replace the address. `grep -rn "prestige-viewer/prestige-viewer"` finds them all.
2. **Your own build server.** 83 of the 85 third-party build packages download from `git.slipstreamviewer.org`. Mirror them (`indra/cmake/3p_manifest.json`) so Prestige builds don't depend on Slipstream's server, and do the same for the speech-to-text models (`llspeechtotext.cpp`).
3. **Updates.** If you want an updater, host your own signed feed and set `UpdateCheckURLRelease`. Until then it stays off.
4. **Talk to the Slipstream developers.** Slipstream is a young, actively developed project (beta, about 60,900 commits), and keeping up with it is easier with their blessing. Tell them about the fork, and ask whether they're happy for Prestige users to share the in-world helper protocol.
5. **Second Life Third-Party Viewer Policy.** Comply with it, and register "Prestige Viewer" in the TPV Directory if you want it listed.
6. **License.** Publish your source (LGPL 2.1) wherever you distribute binaries, and keep the credits and license files.
7. **Code signing.** Sign `PrestigeViewer.exe` and the installer so SmartScreen doesn't block it.

## Staying up to date with Slipstream
Slipstream changes quickly. To pull in its improvements, download a newer `Slipstream-main.zip`, unpack it over the `main` branch, commit, then `git checkout prestige && git merge main`. The `<Prestige>` markers and this file show what to keep when there are conflicts.

## Credits
* Linden Lab: Second Life viewer (LGPL 2.1)
* Sky Line / Bluefast Studios: Slipstream Viewer and its Vulkan renderer (LGPL 2.1)
* Alchemy Development Group: Alchemy Viewer (LGPL 2.1)
* The Phoenix Firestorm Project, Inc.: Firestorm features (LGPL 2.1)
* NiranV Dean: Black Dragon features (LGPL 2.1)
