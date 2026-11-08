# Frost Engine

Frost Engine is an experimental C++20 game engine built from the ground up to recreate the capabilities of the Battlefield 4 era Frostbite engine, using a modern Vulkan-based rendering pipeline. The goal of this project is to support large-scale multiplayer environments, high-performance 3D rendering, advanced physics, and dynamic destruction to act as a foundation for FPS development and experimentation. Frost Engine is built using C++20, Vulkan, GLFW, GLM, and Vulkan Memory Allocator, with CMake and vcpkg for build and dependency management.

Currently in very early development. 

## Targets

- `FrostEngine` (`engine/`): the shared engine library: platform/input, renderer (with depth buffering), camera, asset loading (`AssetManager`), skeletons, skinned meshes and animation clips (`Animation/`: sampling, skinning and playback with `AnimationSystem`; the renderer skins meshes on the GPU), levels (`Level`, `GameObject`, `LevelSerializer`) built on an entity component system (`Registry` and the components in `Scene/Components/`), basic rigid body physics (`PhysicsSystem`), dynamic lighting (directional, point and spot lights, any number per frame), projects (`Project`) and the Dear ImGui host layer. Anything the game also needs (lighting, etc.) belongs here.
- `FrostGame` (`game/`): the game executable. Runs the project's content, staged next to it at build time, and loads `Levels/TestLevel.flevel` at startup, simulating its physics and playing its animations every frame and looking through the level's camera (or a free camera; see [The game](#the-game)). The editor also runs it to play the level being edited (`F5`).
- `FrostEditor` (`editor/`): the level editor executable. Editor-only code (the editor camera, panels, dialogs, Play In Editor) lives here. Building it builds `FrostGame` too, which it starts for Play In Editor. Runs the engine with Dear ImGui enabled (vcpkg `imgui` with `docking-experimental`, `glfw-binding` and `vulkan-binding`; 1.91.9 or newer).

## Projects

A Frost project is one self-contained directory, much like an Unreal Engine `.uproject` folder:

```
MyGame/
  MyGame.frostproject   project descriptor (optional for now)
  content/              the project's Frost assets (.fmesh, .ftex, .fskel, ...), in any folders
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

- **Level** (left): the level's name and objects, as a tree: objects attached to another (see [Attachments](#attachments-and-animated-visibility)) are listed under it. Click to select, double-click to frame, right-click for Focus/Duplicate/Detach/Delete (duplicating or deleting an object takes what is attached to it along). Models (static or skeletal) dropped here are added in front of the camera; a model dropped onto an object is added attached to it. Drag an object onto another to attach it there, or onto the list's empty space to detach it.
- **Inspector** (right): the selected object's name and its components (see below), one section each: *Transform* (position, rotation in degrees, scale with a uniform lock), *Mesh* (the model, and per-part materials), *Attachment* (what the object is attached to, the bone it follows, its slot), *Skeletal Mesh* (the model, its skeleton, what is attached to it, the animation pose and its visibility keys, and per-part materials), *Animation Triggers* (keys that play animations, and which attached objects they show and hide when), *Light*, *Camera*, *Fly Controller*, *Character Controller*, *Box Collider*, *Capsule Collider* and *Rigid Body*. *Add Component* attaches another; the x on a section's header (or right-click > Remove Component) removes it. In the Mesh section, drop a model onto the model slot to assign it; tick "Override material" on a part to edit its color/metallic/roughness/etc. and texture slots; drop an image from the Asset Library onto a slot, or click the slot to pick one, inherit the model's, or clear it. The Skeletal Mesh section works the same way, plus the objects attached to it (and *Add Default Attachments*), which parts of the model itself are attached to which bones (with a *Follow bones* toggle), an *Animation* slot (drop an animation onto it, or click to pick one), a time slider choosing the moment of the animation the model is posed at in the editor (and where the game starts playing it), *Play* to preview the animation in the viewport, the animation's *Visibility keys*, and how the game plays it (*Play in game*, *Loop*, speed). Both have a *Visible* checkbox.
- **Asset Library** (bottom): the *Library* tab shows the content directory as a folder tree (left) and the current folder's contents (right, with breadcrumbs); the search box finds assets and levels in every folder. Levels show up in their folders too (double-click one to open it). *Import...* turns a glTF model (`.gltf`/`.glb`) or image (`.png`/`.jpg`), given by full path or by browsing, into project assets (see [Importing models](#importing-models)). Right-click an asset to reimport, rename, duplicate, delete or locate it; right-click a folder (or empty space) for new folders, renaming and importing there. Drag assets or folders onto a folder (in the tree, the list or the breadcrumbs) to move them. Double-click a skeletal mesh or skeleton to open its editor (see [Skeletal mesh editor](#skeletal-mesh-editor)). Drag models and skeletal meshes into the viewport to place them, textures onto texture slots, and animations onto a skeletal mesh's animation slot. The *File System* tab browses the disk (double-click a source file to import it).
- **Viewport** (center): click to select (the selection is outlined); drop models to place them on whatever is under the cursor (or the ground plane). Box and capsule colliders are drawn as wireframes: green for immovable ones, blue for simulated rigid bodies (Play > Show Colliders turns them off). Cameras are drawn as an icon (click to select) and the shape of their view. A selected skeletal mesh shows its skeleton, in its current pose.

### Objects and components

Every object in a level is an entity with components attached; what an object is and does is entirely down to its components:

| Component | What it does |
|-----------|--------------|
| Transform | Position, rotation and scale. Every object has one (it cannot be removed). |
| Attachment | Attaches the object to another (its *parent*), optionally to a *bone* or *socket* of the parent's skeletal mesh: it moves with the parent and the bone (or socket), and its own transform is relative to its mount. Its *slot* names the role it fills ("Magazine", "Muzzle"), by which the parent's own animation can show and hide it (animation triggers name the objects they show and hide instead). See [Attachments](#attachments-and-animated-visibility). |
| Mesh | A model to draw (a `.fmesh` asset), with optional per-part material overrides, and whether it is *Visible*. Objects placed from a model get one. |
| Skeletal Mesh | A model bound to a skeleton (a `.fskmesh` asset), drawn in a pose of its skeleton: its rest pose, or an *animation* (a `.fanim` asset) at a chosen *time*, e.g. a character placed mid-stride. In the game the animation plays (*Play in game*, looping or not, at a chosen speed); the editor shows the pose at the chosen time. Animations play on a skeleton by bone name, so any animation made for a skeleton with the same bone names works. Parts of the model attached to bones (see [Importing models](#importing-models)) follow them unless *Follow bones* is turned off, when they stay where they were modeled. Per-part material overrides as for a Mesh, and *Visible*. Objects placed from a skeletal mesh get one, with the parts the model was imported with as separate objects attached to them. |
| Animation Triggers | Keys that play animations once on the object's skeletal mesh in the game (`R`: a reload), in place of its own animation until they end. A key pressed while one is still playing is ignored. Each trigger has its own *visibility keys*: which of the objects attached to this one it shows or hides, and from when. |
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
- `.fskel`: a skeleton, imported from glTF: its bones (name, parent, rest pose), and any added in the editor (flagged as such, and kept when it is reimported).
- `.fskmesh`: a skeletal mesh, imported from glTF: geometry in its bind pose, up to four bone influences per vertex, which parts are attached to which bones, the parts imported as separate models (its *default attachments*: each one's mesh, bone and slot), its *sockets* (added in the editor, kept when it is reimported), and materials as in a `.fmesh`, bound to a skeleton asset. Bones are matched by name when it is loaded, so reimporting the skeleton with its bones reordered changes nothing.
- `.fanim`: one animation, imported from one of a glTF file's animations: per animated bone (by name), translation, rotation and scale keyframes (step, linear or cubic spline interpolation, as in the file), plus its slot-based *visibility keys*, authored in the editor (kept when it is reimported).

Every asset has a unique id, stored in its file, and everything refers to assets by id: levels, meshes' materials referring to their textures, and skeletal meshes and animations referring to their skeleton. So assets can be renamed or moved, in the editor or with the operating system's file manager/Git/etc. while the editor is running (the Library picks changes up within a second), without breaking anything; the paths stored alongside the ids are updated the next time a level is saved. Deleting an asset in the editor asks first and lists what still uses it (those uses show as missing, and come back if the asset does). Copying an asset file outside the editor gives two files with the same id: the Library flags the copy, and right-click > *Give Unique Id* makes it a separate asset (*Duplicate* in the editor does this itself).

Importing records where an asset came from (relative to the project directory when the source is inside it, so that survives moving or cloning the project), and for one of several assets made from a file, which part of it (a texture, an animation, a skeletal mesh's separate part), so it can be reimported after the source changes (right-click > *Reimport*; objects using it update immediately; an animation is reimported by itself, from the animation of the same name). Reimporting keeps the asset's id, and updates the textures that came with a model where they are now, even if they were moved or renamed. If the source file has moved, *Reimport From...* reimports from another file. Source files can live anywhere; this repo keeps its sample sources in `assets/`. Assets in the project from before ids (format version 1, with textures embedded in meshes) are upgraded in place when the editor starts. Old levels that point at `.gltf`/`.png` source files directly show those objects as missing: import the files and assign the resulting assets.

The editor works on the project directly, so imports and saved levels become part of it. The game loads the copy of the project's `content/` staged next to its executable, refreshed on every build.

### Importing models

A glTF model can become any combination of these assets, chosen in the *Import* dialog (which first shows what the file contains: its meshes, the skeleton it makes, and its animations):

- *Static mesh* (`.fmesh`): the whole model as posed in the file, as one rigid mesh. A skinned model is posed by its skin (its joints where the file puts them).
- *Skeletal mesh* (`.fskmesh`): the model bound to a skeleton. Skinned meshes keep their skins; any other mesh is a rigid part *attached* to a bone, which it moves with. A part under a bone in the file follows that bone (a weapon parented to a hand bone follows the hand). A *loose* part, one with no bone above it (exporters leave parts attached with constraints, such as Blender's *Child Of*, like that: a magazine next to the armature rather than under a bone), is attached to a suggested bone: one whose name shares a word with the part's, preferring bones that deform nothing since those exist to carry parts (`AK12Mag` goes to `MagEquipped` rather than `MagCatch`), else the skeleton's root joint (`AK12MuzzleBrake` goes to `Root`). Each part is either *part of the mesh* (baked into the skeletal mesh, following its bone) or a *separate object*: imported as a model of its own, `<skeletal mesh>_<part>.fmesh` next to the skeletal mesh, modeled where it sits on the skeletal mesh, and placed as an object attached to it (following the same bone) whenever the skeletal mesh is placed in a level, so it can be removed or replaced there (see [Attachments](#attachments-and-animated-visibility)). Loose parts are separate objects by default, parts under a bone part of the mesh. The dialog's *Attached parts* list shows each part's choice and bone, lets you choose others, or turns attaching loose parts off; reimporting keeps the choices (and the separate parts' files and ids).
- *Skeleton* (`.fskel`): the bones are every joint of the file's skins and every node an animation moves, plus the nodes above them (e.g. Blender's `Armature`). A file without skins makes a skeleton of its node hierarchy, so a model animated by moving its parts (a weapon's slide, a door) can be imported as a skeletal mesh with its animations too. Instead of the file's skeleton, the skeletal mesh and animations can be bound to an *existing* skeleton in the project, by bone name: that is how a file with only animations (an animation pack exported from the same rig) is imported for a character imported earlier.
- *Animations* (`.fanim`): each of the file's animations, ticked one by one, becomes an asset of its own (by default in the `Animations` folder next to the model's `Meshes` folder). Channels for nodes the skeleton has no bone for, and morph target weights (not supported), are left out, with a warning.

A model with a skin or animations is imported as a skeletal mesh with its skeleton and all its animations by default, any other as a static mesh; the dialog shows, per asset, whether it is new or updates what an earlier import of the file made (which keeps its id). Everything is read before anything is written, so a problem with any part leaves the project as it was. Lengths are converted from glTF's meters to centimeters. `assets/RiggedArm.glb` is a small sample: a three-bone arm with *Wave* and *Twist* animations.

Skeletal meshes are skinned on the GPU: each frame, a pose is sampled from the animation and its skinning matrices (up to four bones per vertex, plus one per attached part) go to the vertex shader (`shaders/skinned.vert`), so animations play at full frame rate in the game (and in the editor's preview). Picking and bounds use the same pose, computed on the CPU when needed.

### Attachments and animated visibility

An object can be attached to another: it then moves with it, and if a bone of the other's skeletal mesh is chosen, with that bone. An attached object's transform is relative to its *mount*, the bone's rest pose on the parent, so a part modeled in the parent's space, with the same origin (as a weapon's magazine, muzzle device and sights usually are in Blender), sits exactly where it was modeled with an identity transform, and stays there as the bone moves: no socket offsets to set up. Attaching an object (dragging it onto another in the Level panel, or picking a *Parent* in its Attachment section) puts it on its mount; detaching it leaves it where it is. Attached objects are left out of physics, and hiding an object hides what is attached to it.

This is how a weapon carries its attachments: placing the AK12's skeletal mesh places its magazine and muzzle brake as objects attached to it (to the bones `MagEquipped` and `Root`). Delete one to remove it, or drop another model onto its Mesh slot to replace it (an extended magazine, a suppressor): the replacement keeps the bone and slot.

An *animation trigger* (a key that plays an animation once, such as a reload) shows and hides particular objects while its animation plays: its *visibility keys* each name one of the objects attached to the weapon (by the object itself, whatever its name or slot, so several copies of the same magazine are told apart) and say, from a moment on, whether it is shown or hidden. Before an object's first key, and whenever the animation is not playing, it is as its *Visible* checkbox says. The keys are edited per trigger in the Animation Triggers section: pose the model at a moment (the slider next to *Preview*), then *Add Key at ...* and pick the object and Show/Hide. They are saved with the level; duplicating the weapon gives the copy keys for the copies of its parts, and deleting a part deletes its keys.

An animation played as a skeletal mesh's *own* animation (not by a trigger) can also show and hide attachments, by *slot*: those keys belong to the animation asset (edited in the Skeletal Mesh section, saved to the `.fanim` right away and kept by a reimport), and do not apply when a trigger plays the animation.

For example, an AK12 with an empty reload on `R`, where the magazine in the hand is shown while it is held and the one in the weapon when it is seated:

1. Import `AK12.glb` (its magazine and muzzle brake separate, as by default) with its animations, and place the skeletal mesh. It comes with `AK12Mag` on `MagEquipped`.
2. Make three magazines (select it, *Duplicate* twice: copies of an attached object stay on its mount), and give them their bones, names and visibility: `AK12Mag` on `Root`, visible (the one seated in the weapon, which moves with the weapon's recoil); `MagEquipped` on `MagEquipped`, hidden (pulled out by the reload); `MagSpare` on `MagSpare`, hidden (brought in by the reload). They can all keep the same slot.
3. On the weapon, add *Animation Triggers*, *Add Trigger*: `R`, animation `AK12_Empty_Reload`.
4. Its visibility keys: at 0 s, `AK12Mag` hidden and `MagEquipped` shown; at 0.8 s (when it has come out), `MagEquipped` hidden and `MagSpare` shown; at 2.4 s (the end), `MagSpare` hidden and `AK12Mag` shown. *Preview* plays it in the editor.

In the game, `R` then plays the reload once and swaps the magazines as it goes; at the end the weapon is back to its own animation (or rest pose) with the seated magazine showing.

### Skeletal mesh editor

Double-clicking a skeletal mesh or skeleton in the Asset Library (or right-click > *Edit...*) opens its editor in a window of its own (one per asset; close it with its x, which asks first if there are unsaved changes):

- **Skeleton tree** (left): the bones, with the skeletal mesh's sockets under the bones they follow, and those on the mesh itself under the mesh. Bones added in the editor are shown in blue. Right-click a bone to add a socket or a bone under it, or to delete an added bone; right-click a socket to delete it.
- **Preview** (center): the model (with the parts it comes with, such as the AK12's magazine and muzzle brake), flat shaded, with its skeleton and its sockets (a marker with the socket's axes: red x, green y, blue z). Pick an animation in the toolbar to pose it (with *Play* or a time slider) and see the sockets follow their bones. Drag to orbit, use the wheel to zoom, *Frame* to reset; click a joint or socket to select it.
- **Details** (right): the selected socket's name, the bone it follows (or the mesh itself) and its offset from it (position in centimeters along the bone's axes, rotation, scale; the bone's own scale does not apply, so a rig scaled in its armature still has true-to-size offsets). For a bone: its name and parent and its rest pose (editable for bones added in the editor; imported bones are as the model has them).

*Add Socket* adds one on the selected bone (or on the mesh, if no bone is selected); *Add Bone* adds a bone under the selected bone (or a new root). Edits are made to a working copy: *Save* writes them (sockets into the `.fskmesh`; bones into its skeleton's `.fskel`, which every skeletal mesh and animation bound to it shares), and the level shows them right away; *Revert* drops them. A skeleton's editor edits bones only (sockets belong to skeletal meshes), with a skeletal mesh bound to it as the preview. Reimporting keeps both: an added bone stays as long as the model still has its parent bone.

An object attached to a socket (pick the socket under *Sockets* in the Attachment section's *Bone* list) sits at the socket: its transform is relative to the socket, so a part modeled at its own origin (a suppressor) goes on a *Muzzle* socket as it is. (Attaching to a bone instead keeps a part modeled in the weapon's space where it was modeled; see above.) In code, `SkeletalMeshComponent::GetSocketTransform(name)` gives a socket's current transform, for gameplay code (and, later, scripts) to use.

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

Objects with *Animation Triggers* play their animations on their keys (e.g. `R` to reload).

Started from the editor, `F5` (and `Esc`, when it is not giving back the mouse) closes the game.
