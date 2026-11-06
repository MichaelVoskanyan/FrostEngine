# Frost Engine

Frost Engine is an experimental C++20 game engine built from the ground up to recreate the capabilities of the Battlefield 4 era Frostbite engine, using a modern Vulkan-based rendering pipeline. The goal of this project is to support large-scale multiplayer environments, high-performance 3D rendering, advanced physics, and dynamic destruction to act as a foundation for FPS development and experimentation. Frost Engine is built using C++20, Vulkan, GLFW, GLM, and Vulkan Memory Allocator, with CMake and vcpkg for build and dependency management.

Currently in very early development. 

## Targets

- `FrostEngine` (`engine/`): the shared engine library: platform/input, renderer (with depth buffering), camera, asset loading (`AssetManager`), levels (`Level`, `GameObject`, `LevelSerializer`) built on an entity component system (`Registry` and the components in `Scene/Components/`), basic rigid body physics (`PhysicsSystem`), dynamic lighting (directional, point and spot lights, any number per frame), projects (`Project`) and the Dear ImGui host layer. Anything the game also needs (lighting, etc.) belongs here.
- `FrostGame` (`game/`): the game executable. Runs the project's content, staged next to it at build time, and loads `Levels/TestLevel.flevel` at startup, simulating its physics every frame and looking through the level's camera (or a free camera; see [The game](#the-game)). The editor also runs it to play the level being edited (`F5`).
- `FrostEditor` (`editor/`): the level editor executable. Editor-only code (the editor camera, panels, dialogs, Play In Editor) lives here. Building it builds `FrostGame` too, which it starts for Play In Editor. Runs the engine with Dear ImGui enabled (vcpkg `imgui` with `docking-experimental`, `glfw-binding` and `vulkan-binding`; 1.91.9 or newer).

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

## Units

World space is measured in centimeters: one unit is one centimeter, as in Unreal Engine. Positions, sizes and distances (collider sizes, light ranges, camera clip planes) are in centimeters and speeds in centimeters per second; mass is in kilograms, time in seconds and angles in degrees. In code, write lengths with the constants in `engine/include/Frost/Units.hpp` (`1.8f * kMeter`, `35.0f * kCentimeter`) so the unit is visible. The Inspector shows lengths in cm.

Light intensities are still given at 1 meter from the light (a point light of intensity 20 is as bright at 1 m as before), and gravity is 981 cm/s².

glTF models are in meters, so importing one converts it to centimeters (a 1.8 m tall model becomes 180 units tall). Files from before centimeters are converted as they are read, so nothing needs redoing: levels saved before (format versions up to 6, in meters) open at the same size, and are saved in centimeters from then on; meshes imported before (`.fmesh` versions 1 and 2) load at the same size (reimporting one writes it in centimeters).

## Editor

The editor opens with a docked layout around the 3D viewport (Window > Reset Layout restores it):

- **Level** (left): the level's name and objects. Click to select, double-click to frame, right-click for Focus/Duplicate/Delete. Models dropped here are added in front of the camera.
- **Inspector** (right): the selected object's name and its components (see below), one section each: *Transform* (position, rotation in degrees, scale with a uniform lock), *Mesh* (the model, and per-part materials), *Light*, *Camera*, *Fly Controller*, *Character Controller*, *Box Collider*, *Capsule Collider* and *Rigid Body*. *Add Component* attaches another; the x on a section's header (or right-click > Remove Component) removes it. In the Mesh section, drop a model onto the model slot to assign it; tick "Override material" on a part to edit its color/metallic/roughness/etc. and texture slots; drop an image from the Asset Library onto a slot, or click the slot to pick one, inherit the model's, or clear it.
- **Asset Library** (bottom): the *Library* tab shows the content directory as a folder tree (left) and the current folder's contents (right, with breadcrumbs); the search box finds assets and levels in every folder. Levels show up in their folders too (double-click one to open it). *Import...* turns a glTF model (`.gltf`/`.glb`) or image (`.png`/`.jpg`), given by full path or by browsing, into project assets (see below). Right-click an asset to reimport, rename, duplicate, delete or locate it; right-click a folder (or empty space) for new folders, renaming and importing there. Drag assets or folders onto a folder (in the tree, the list or the breadcrumbs) to move them. Drag models into the viewport to place them, and textures onto texture slots. The *File System* tab browses the disk (double-click a source file to import it).
- **Viewport** (center): click to select (the selection is outlined); drop models to place them on whatever is under the cursor (or the ground plane). Box and capsule colliders are drawn as wireframes: green for immovable ones, blue for simulated rigid bodies (Play > Show Colliders turns them off). Cameras are drawn as an icon (click to select) and the shape of their view.

### Objects and components

Every object in a level is an entity with components attached; what an object is and does is entirely down to its components:

| Component | What it does |
|-----------|--------------|
| Transform | Position, rotation and scale. Every object has one (it cannot be removed). |
| Mesh | A model to draw (a `.fmesh` asset), with optional per-part material overrides. Objects placed from a model get one. |
| Light | A directional, point or spot light at the object's position, shining along its forward direction. |
| Camera | A viewpoint the game looks through, with a field of view and near/far clip distances. It is mounted on its object: at an *offset* from it and with its own *rotation* on top of the object's (by default neither: at the object, looking along its forward direction), so it moves and turns with it, e.g. at a character's eye height. *Add... > Camera* in the Level panel places one exactly where the viewport is looking from; *Align with View* moves an existing one (with its object) there. |
| Fly Controller | Flies its object around in the game from the player's input (mouse to turn, W/A/S/D etc. to move; no gravity or collision), at a set speed and mouse sensitivity. On a camera, it makes a free camera; a stand-in for a player character until there is one. |
| Character Controller | Walks its object around like a first-person character, from the player's input: mouse left/right turns the body, mouse up/down tilts only its Camera, W/A/S/D walk on the ground plane relative to where the body faces, Space jumps (only when standing on something). The object needs a simulated Rigid Body and a collider (physics makes it fall, land and stop at walls) and a Camera to see through. *Add... > Character* in the Level panel places a ready-made one. |
| Box Collider | A solid box in the object's own space (center and size; it moves, turns and scales with the object). Added to an object with a model, it starts out fitted to the model (*Fit to Model* refits it). |
| Capsule Collider | A solid capsule (a cylinder with rounded ends) standing along the object's up axis: center, radius and total height. The usual shape for a character. An object collides with one shape: if it also has a Box Collider, the box is used. |
| Rigid Body | *Simulated*: the object falls under gravity and, with a collider, collides. *Mass* matters when two simulated bodies collide. |

In code, components are plain types stored per type in the level's `Registry`; `GameObject` is the handle (`AddComponent<T>()`, `GetComponent<T>()`, `TryGetComponent<T>()`, `HasComponent<T>()`, `RemoveComponent<T>()`), and systems walk every entity with a given set of components with `registry.Each<A, B>(...)`. To add a component type, follow the steps in `engine/include/Frost/Scene/Components/Components.hpp` (define it, add it to `BuiltinComponents`, give it a `ComponentSerializer` for level files and a `DrawComponent` for the Inspector; the build fails until both exist).

### Physics

Objects whose Rigid Body has *Simulated* ticked fall under gravity (981 cm/s², i.e. 9.81 m/s², down -Y). A simulated object with a collider (box or capsule) collides with every other collider: it stops on (and slides, with friction, along) colliders of objects that are not simulated, which never move, so a large box collider with no rigid body (or an unticked one) makes a ground; two simulated objects push each other apart according to their masses (a character pushes a light box along). Without a collider, a simulated object falls through everything. The simulation also records whether each body is standing on something (what a character checks before jumping). Rotation is not simulated. The simulation steps at a fixed 120 Hz whatever the frame rate.

The game simulates its level from the start, and so does playing a level from the editor (`F5`, see below). Play > Simulate Physics Here (`Alt+S`) runs the simulation in the editor's own viewport instead; stopping it (again `Alt+S`, or saving) puts every object back where it was when it started, so simulating never changes the level.

`content/Levels/PhysicsTest.flevel` tries out everything together. The ground is a 40 m x 1 m x 40 m platform (the 1 m cube model `content/Primitives/Cube.fmesh`, imported from `assets/Primitives/Cube.gltf`, scaled up, with a Box Collider). The Glock, with a fitted Box Collider and a simulated Rigid Body, starts 2 m above it. *Player* is the player character: an object with no model, just a simulated Rigid Body (80 kg), a Capsule Collider (180 cm tall, 35 cm radius), a Character Controller and a Camera mounted 160 cm above its feet, starting 8 m behind the Glock, facing it. Open the level in the editor and press `F5`: both drop onto the ground, and you walk around as the player (click in the game window to look around with the mouse, see [The game](#the-game)): walk up to the Glock and push it, jump, or walk off the edge.

### Play In Editor

`F5` (or Play > Play in Game Window) plays the level being edited in the game, `FrostGame`, in a window of its own, much like Unreal's standalone Play In Editor. The game gets the level as it is at that moment, unsaved changes included (the editor saves a snapshot to a temporary file, outside the project), and runs on the editor's project directly, so assets imported a moment ago are there too. If the level has no camera, the game's free camera starts where the editor's viewport is looking from.

`F5` again, in the editor or in the game window, stops the game and closes its window (so does `Esc` in the game, or closing its window). The editor shows *PLAYING* in the viewport and *[Playing]* in its title meanwhile, and editing carries on as usual; what happens in the game never comes back into the editor. If the game fails to start or exits with an error (e.g. a model it cannot load), the editor says so; the details are in the console, which the game shares with the editor. Quitting the editor closes the game.

The editor runs the `FrostGame` it was built with (for the same configuration).

### Lights

The Asset Library's *Lights* tab holds directional, point and spot lights: drag one into the viewport to place it (point lights float just above the surface, spot lights hang above it aiming down), or use the Level panel's *Add...* menu. Lights are shown in the viewport as colored icons (click to select); the selected light shows its reach (radius sphere, spot cone, or sun rays). Aim directional and spot lights by rotating their object.

The Inspector edits a light's type, on/off, color (or a color temperature in Kelvin), intensity, radius (point) / distance (spot), and the spot's inner and outer cone angles; the viewport updates immediately. The Level panel's *Environment* section sets the level's ambient light. Any object can also carry a light (*Add Component > Light* in the Inspector), e.g. a lamp model.

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

Releasing the button gives the cursor back. Other shortcuts: `F` frames the selection, `Delete` deletes it, `Ctrl+D` duplicates it, `F5` plays the level in the game (and stops it), `Alt+S` starts/stops the physics simulation in the viewport.

## The game

`FrostGame [--project <project>] [--level <level>] [--view <x,y,z,yaw,pitch>] [--play-in-editor]` (`--help` lists these). Without arguments it runs the project staged in its working directory and plays `Levels/TestLevel.flevel`.

It looks through the level's camera: the first object with a Camera component. A camera stays where the level puts it (or goes where its object goes): with a Character Controller on its object, that is where the player walks it; with a Fly Controller, where the player flies it. Each controller reads the player's input every frame. A level without a camera gets one for the run: a *Free Camera* object with a Camera and a Fly Controller, which starts out looking at the whole level (or from `--view`, which is what the editor passes). Flying an object with a Fly Controller:

| Input                | Action                              |
|----------------------|-------------------------------------|
| Click in the window  | Take the mouse (to look around; only if something has a Fly Controller) |
| Mouse                | Look around (while it has the mouse)|
| `Esc`                | Give the mouse back                 |
| `W` / `S`            | Move forward / backward             |
| `A` / `D`            | Move left / right                   |
| `Space` / `Left Ctrl`| Move up / down (world)              |
| `Left Shift`         | Move faster                         |
| Mouse wheel          | Increase / decrease speed           |

Walking a character (Character Controller):

| Input                | Action                                          |
|----------------------|-------------------------------------------------|
| Click in the window  | Take the mouse (to look around)                 |
| Mouse left / right   | Turn the body (while it has the mouse)          |
| Mouse up / down      | Look up / down (the camera only)                |
| `W` / `S`            | Walk forward / backward                         |
| `A` / `D`            | Walk left / right                               |
| `Space`              | Jump (when standing on something)               |
| `Esc`                | Give the mouse back                             |

Started from the editor, `F5` (and `Esc`, when it is not giving back the mouse) closes the game.
