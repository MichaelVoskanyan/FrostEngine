# Frost Engine

Frost Engine is an experimental C++20 game engine built from the ground up to recreate the capabilities of the Battlefield 4 era Frostbite engine, using a modern Vulkan-based rendering pipeline. The goal of this project is to support large-scale multiplayer environments, high-performance 3D rendering, advanced physics, and dynamic destruction to act as a foundation for FPS development and experimentation. Frost Engine is built using C++20, Vulkan, GLFW, GLM, and Vulkan Memory Allocator, with CMake and vcpkg for build and dependency management.

Currently in very early development. 

## Targets

- `FrostEngine` (`engine/`): the shared engine library: platform/input, renderer (with depth buffering), camera, asset loading (`AssetManager`), levels (`Level`, `GameObject`, `LevelSerializer`), dynamic lighting (directional, point and spot lights, any number per frame), projects (`Project`) and the Dear ImGui host layer. Anything the game also needs (lighting, etc.) belongs here.
- `FrostGame` (`game/`): the game executable. Runs the project's content, staged next to it at build time, and loads `Levels/TestLevel.flevel` at startup.
- `FrostEditor` (`editor/`): the level editor executable. Editor-only code (the editor camera, panels, dialogs) lives here. Runs the engine with Dear ImGui enabled (vcpkg `imgui` with `docking-experimental`, `glfw-binding` and `vulkan-binding`; 1.91.9 or newer).

## Projects

A Frost project is one self-contained directory, much like an Unreal Engine `.uproject` folder:

```
MyGame/
  MyGame.frostproject   project descriptor (optional for now)
  content/              the project's Frost assets (.fmesh, .ftex), in any folders
    Levels/             its levels (.flevel); new levels are saved here by default
  assets/               source files (glTF, PNG, ...), by convention
```

This repository is itself the sample project (`content/`, with sources in `assets/`); the CMake cache variable `FROST_PROJECT_DIR` picks the project the game is built for and the editor opens by default.

A project only ever uses its own content. Assets are found by id inside the project's `content/` and nowhere else; levels are opened from and saved into it; nothing is read from another project, and nothing is imported behind your back (importing a source file is always an explicit *Import...*). Sharing assets between projects is a file copy: copy a model's folder (its `.fmesh` and the `.ftex` files it uses) into another project's `content/`, anywhere, and it works there (references are by id). Assets in another project's folder cannot be placed directly; the editor says to copy them in first.

Open a project in the editor with `FrostEditor <project directory>` or `FrostEditor <file>.frostproject` (or `--project <path>`); with no argument it opens `FROST_PROJECT_DIR`. A directory is a project if it has a `content/` directory or a `.frostproject` file, so to start a new project, create a folder with an empty `content/` in it (the editor adds `content/Levels/`). The `.frostproject` file names the project; its contents are not read yet (project settings such as the startup level will live there).

## Editor

The editor opens with a docked layout around the 3D viewport (Window > Reset Layout restores it):

- **Level** (left): the level's name and objects. Click to select, double-click to frame, right-click for Focus/Duplicate/Delete. Models dropped here are added in front of the camera.
- **Inspector** (right): the selected object's name, transform (position, rotation in degrees, scale with a uniform lock), model, and per-part materials. Tick "Override material" on a part to edit its color/metallic/roughness/etc. and texture slots; drop an image from the Asset Library onto a slot, or click the slot to pick one, inherit the model's, or clear it.
- **Asset Library** (bottom): the *Library* tab shows the content directory as a folder tree (left) and the current folder's contents (right, with breadcrumbs); the search box finds assets and levels in every folder. Levels show up in their folders too (double-click one to open it). *Import...* turns a glTF model (`.gltf`/`.glb`) or image (`.png`/`.jpg`), given by full path or by browsing, into project assets (see below). Right-click an asset to reimport, rename, duplicate, delete or locate it; right-click a folder (or empty space) for new folders, renaming and importing there. Drag assets or folders onto a folder (in the tree, the list or the breadcrumbs) to move them. Drag models into the viewport to place them, and textures onto texture slots. The *File System* tab browses the disk (double-click a source file to import it).
- **Viewport** (center): click to select (the selection is outlined); drop models to place them on whatever is under the cursor (or the ground plane).

### Lights

The Asset Library's *Lights* tab holds directional, point and spot lights: drag one into the viewport to place it (point lights float just above the surface, spot lights hang above it aiming down), or use the Level panel's *Add...* menu. Lights are shown in the viewport as colored icons (click to select); the selected light shows its reach (radius sphere, spot cone, or sun rays). Aim directional and spot lights by rotating their object.

The Inspector edits a light's type, on/off, color (or a color temperature in Kelvin), intensity, radius (point) / distance (spot), and the spot's inner and outer cone angles; the viewport updates immediately. The Level panel's *Environment* section sets the level's ambient light. Any object can also carry a light (*Add Light* in the Inspector), e.g. a lamp model.

Levels are `.flevel` JSON files: File > New / Open / Save / Save As (Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S). A new level is saved in the project's `content/Levels/` by default (any folder inside `content/` works; outside it, the dialogs refuse, as the level would not be part of the project). Levels refer to assets by id (plus the path, relative to the level file, as a fallback and for readability), so a project folder can be moved. The editor asks before discarding unsaved changes.

### Project content and assets

A project's content lives in its `content/`, organized in whatever folders you like (this repo uses e.g. `Weapons/Glock19/Meshes` and `Weapons/Glock19/Textures`), with levels in `Levels/`. Levels and the game only ever load Frost assets, never source files:

- `.fmesh`: a mesh (vertices, indices, parts and materials), imported from glTF. Its materials refer to texture assets.
- `.ftex`: a texture, imported from PNG/JPEG, or extracted from a model when it is imported: each texture a model comes with becomes a standalone `.ftex` (by default in the `Textures` folder next to the model's `Meshes` folder, or a `Textures` subfolder), usable on any object.

Every asset has a unique id, stored in its file, and everything refers to assets by id: levels, and meshes' materials referring to their textures. So assets can be renamed or moved, in the editor or with the operating system's file manager/Git/etc. while the editor is running (the Library picks changes up within a second), without breaking anything; the paths stored alongside the ids are updated the next time a level is saved. Deleting an asset in the editor asks first and lists what still uses it (those uses show as missing, and come back if the asset does). Copying an asset file outside the editor gives two files with the same id: the Library flags the copy, and right-click > *Give Unique Id* makes it a separate asset (*Duplicate* in the editor does this itself).

Importing records where an asset came from (relative to the project directory when the source is inside it, so that survives moving or cloning the project), so it can be reimported after the source changes (right-click > *Reimport*; objects using it update immediately). Reimporting keeps the asset's id, and updates the textures that came with a model where they are now, even if they were moved or renamed. If the source file has moved, *Reimport From...* reimports from another file. Source files can live anywhere; this repo keeps its sample sources in `assets/`. Assets in the project from before ids (format version 1, with textures embedded in meshes) are upgraded in place when the editor starts. Old levels that point at `.gltf`/`.png` source files directly show those objects as missing: import the files and assign the resulting assets.

The editor works on the project directly, so imports and saved levels become part of it. The game loads the copy of the project's `content/` staged next to its executable, refreshed on every build.

### Editor controls

Hold the right mouse button over the viewport to fly the camera. While it is held the cursor is captured and hidden, and:

| Input                | Action                      |
|----------------------|-----------------------------|
| Mouse                | Look around                 |
| `W` / `S`            | Move forward / backward     |
| `A` / `D`            | Move left / right           |
| `Space` / `Left Ctrl`| Move up / down (world)      |
| Mouse wheel          | Increase / decrease speed   |

Releasing the button gives the cursor back. Other shortcuts: `F` frames the selection, `Delete` deletes it, `Ctrl+D` duplicates it.
