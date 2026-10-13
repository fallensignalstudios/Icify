# Icify

Procedural ice for Unreal Engine 5. Drop a **Freeze Zone** into a level, size the box, press **Preview**, and
everything inside it freezes the way it would in real weather:

- **Frost**: hoarfrost on faces open to the sky, heavy on exposed edges, rime feathers growing into the wind, a band
  of ground frost where the coldest air settles. On glass it grows into fern frost.
- **Snow**: only where snow can actually fall and stay. The zone traces the sky along the snowfall direction, so
  anything under a roof, a balcony, an awning or a tree stays clear. Steep faces shed it; corners drift; windward walls
  get plastered.
- **Glaze**: a clear freezing-rain coat, frozen drips running down from gutters, spouts and hand-placed melt sources,
  and a ring of refrozen meltwater around anything warm.
- **Black ice**: thin dark sheets on open floors, ice in floor creases and frozen puddles where meltwater collects.
- **Icicles**: real geometry. Icify finds every drip edge on your meshes (eaves, ledges, sills, gutters, the lowest
  line of a pipe, the bottom of a lamp globe), checks that meltwater can reach it and that there is room to hang, and
  grows clumps of rippled, rimed, slightly crooked icicles there.

Heat sources (fires, heaters, chimneys, and optionally lamps) thaw everything around them. The same seed and settings
always give the same result, every edit is one undo step, and a preview is never saved.

Icify is built on the same foundations as Grungify, our weathering plugin. If you know one, you know the other. They
use separate custom primitive data slots and separate material functions, so a mesh can be weathered and frozen at the
same time.

> **Status: 0.1.0 beta, not yet compiled in Unreal Engine.** Icify was written without an engine install. Everything
> that can run without one has been checked: the shader code compiles under glslang for pixel and vertex stages, the
> icicle geometry builder passes 45 offline geometry checks with clang and gcc, and the drip-edge rules were prototyped
> and checked on eleven test shapes (boxes, bevels, fillets, pipes, sloped slabs and roofs, a sphere). The engine-facing code mostly reuses Grungify's, which was verified on UE 5.8.3. The parts
> that are new (the procedural icicle components, the sky cover field and the generated icicle material) still need
> their first compile. See [First compile](#first-compile) and `Docs/EngineCompatibility.md`.

---

## Quick start

1. Copy the `Icify` folder into your project's `Plugins/` folder and open the project. The first time the editor
   starts, Icify builds its content in `/Icify` (`MF_IcifyFrost`, `M_IcifyIcicle`, `BP_FreezeZone` and friends) and
   writes `Saved/Icify/ContentReport.txt`.
2. Select the actors you want frozen and choose **Tools > Icify > Freeze Selected Actors** (or right-click them >
   **Freeze with Icify**).
3. If their materials don't use Icify yet, you'll be asked. **Yes** makes private copies of their master materials
   for just those actors, so nothing shared changes. **No** hooks up the shared masters themselves, each one backed up
   first. Icicles grow either way, because they have their own material.
4. A zone fitted to the selection previews the result. Play with the settings in the Details panel; with **Live
   Preview** on, every change regenerates.
5. Press **Apply** to keep it, or **Cancel Preview** to drop it. **Clear** puts everything back exactly as it was.

To freeze a whole area instead, place `BP_FreezeZone` (or the native **Icify Freeze Zone**) from the Place Actors
panel, scale its box over the area, and run **Tools > Icify > Hook Up Materials In Level** once.

---

## How it works

### Masks

For every static mesh it overlaps, the zone evaluates four ice layers and bakes them into the mesh:

| Channel | Layer | Where it forms |
|---|---|---|
| R | Frost | faces open to the sky, exposed convex edges, windward faces (rime), near the ground |
| G | Snow | up-facing surfaces below the slope limit that snow can fall onto; creases; windward walls |
| B | Glaze | open faces (freezing rain), below melt sources, along traced meltwater, around thawed patches |
| A | Black ice | open floors, floor creases, puddles under melt sources and where traced meltwater pools |

They go into the mesh's **vertex colours**, or into a small per-instance **mask texture** in the mask UV channel (UV1
by default). Auto picks the texture for Nanite meshes and sparse geometry, where vertex colours can't carry the detail.
The zone binds a material instance of its own to each frozen slot. That instance is the only thing that switches the
material's ice on (`Icify Active`), so meshes the zone hasn't touched keep their exact original look, even when they
share the material.

`MF_IcifyFrost` turns the masks into shading at the zone's physical feature scale. Thin frost is a dusting of sugary
grain and needle crystals with the surface showing through. It grows in patches with feathered edges and closes into a
solid coat as its mask rises. The needles lie flat on the surface in every direction, in domains that blend without
seams on any surface orientation. Snow breaks up into crisp-edged patches, buries the surface detail and gets a soft
drifted relief. Glaze darkens and deepens what's under it and turns it glossy. Black ice forms dark, glassy sheets
with clean edges. Frost and snow sparkle: small round facets tilt and turn glossy, so they catch real lights at their
own angles. Crystal detail finer than a pixel fades to its average with distance (estimated from the camera distance,
since no `ddx`/`ddy` is used anywhere, so it all works with Nanite compute shading), and the sparkle fades out
between 6 and 24 m, so nothing shimmers far away.

### The sky

When **Use Sky Cover** is on, the zone shoots a grid of traces along the snowfall direction: straight down, tilted by
**Snowfall Slant** towards the wind. It records where each one first hits something. That's a shadow map of the sky.
A surface gets snow, frost film and freezing rain only if it's open in that map, so a roof or a canopy keeps everything
under it dry, even when it sits above the zone box (**Sky Reach**). Faces turned away from the fall read as sheltered.
Actors tagged `Icify.Ignore` don't shelter anything.

### Drip edges and icicles

Water runs downhill over a mesh until it reaches a point with nowhere lower to go, and drips from there. Icify finds
those points on each mesh's welded LOD0 topology:

- the sinks, where water arrives from above and the surface turns under by more than **Drip Overhang**;
- the convex creases that join them (the front edge of a ledge, the eave of a sloped roof, the lowest line of a pipe);
- open borders that water flows off.

A lone sink, like the bottom of a sphere, becomes a single drip point. Candidates are spaced along every drip edge.

Each candidate then has to earn its icicle:

1. **Meltwater**: snow must lie above it (the first thing the sky sees above the edge is the mesh itself, or something
   resting right on it), a melt source must feed it, or it must belong to a gutter or downpipe (`Icify.Drip` or
   named like one). Turn **Icicles Need Meltwater** off to let every drip edge grow.
2. **Thaw**: nothing within a heat source's thaw radius.
3. **Stamps**: freeze stamps with a positive **Icicles** layer add icicles and the meltwater for them; negative ones
   erase them.
4. **Density**: about **Icicle Density** icicles per metre of fully fed edge. **Icicle Clumping** gathers them into
   long clumps with bare gaps between, like real eaves. Edges with more meltwater grow longer icicles.
5. **Spacing**: the longest icicles claim their place first; smaller ones fill the gaps.
6. **Room**: a trace off the edge rejects edges against a wall (the back of a ledge, a sill in its reveal). A trace
   down stops each icicle short of whatever is below it.

The survivors are built by a deterministic geometry builder. Each has a flared root that wraps into a bead of ice
along the edge, freeze ripples, a slight sag and kink, and a sharp tip. Vertex colour carries rime, thickness,
root-to-tip and a per-icicle random into `M_IcifyIcicle`, a subsurface ice material that glows through thin tips and
turns white where rimed. The icicles live in two procedural mesh components on the zone:

- **Icicles**: the applied ones, saved with the level and rebuilt on every Apply.
- **PreviewIcicles**: never saved. It is emptied before every save, and while a preview is up it stands in for the
  applied icicles, which are hidden.

Icicles stay put when you move the zone box; they belong to the meshes they hang from.

---

## The Freeze Zone

Every setting has a tooltip; this is the short tour. Actions are at the top of the Details panel: **Preview**,
**Apply**, **Clear**, **Cancel Preview**, **New Seed**, **Dry Run** (report what would happen, touch nothing),
**Load Preset**, **Save To Preset** and **Clear Generation Cache**.

| Category | What's there |
|---|---|
| Icify | Seed, overall Intensity, Feature Scale, Surface Type (Auto, or force Stone / Metal / Glass), Live Preview, Preview Display (the final material, or each mask on the debug material), mask output (Auto / Vertex Color / Texture), texel size and resolution, report files |
| Weather | Wind Direction (the blue arrow), Wind Influence, Snowfall Slant, Use Sky Cover, sky trace spacing and reach |
| Frost | Film on open faces, Edge Frost, Rime into the wind, Ground Frost and its height, edge angle and width |
| Snow | Max slope, Crevice Snow, Wall Plaster, Edge Snow Loss |
| Glaze & Drips | Glaze Coat, Ambient Drips, drip length and spread, hand-placed Melt Sources (drag the diamonds in the viewport), auto-detected drip sources, Drip Source Actors, Trace Meltwater (follow the water down the real geometry into puddles) |
| Black Ice | Floor Ice, Crevice Ice, Puddle Radius |
| Icicles | Grow Icicles, Density, Max / Min Length, Thickness, Ripple spacing, Clumping, Lean (with the wind), Drip Overhang, Icicles Need Meltwater, Max Icicles, Sides, Collision, Shadows, Icicle Material |
| Thaw | Thaw Intensity and radius, thaw from lights, Heat Source Actors |
| Surfaces | Stone, Metal and Glass profiles (frost, snow, glaze, black ice and icicles per family), named Material Profiles for slot rules |
| Palette | Material look written onto the zone's own instances: frost and snow colours, ice tint, per-layer strengths, relief, sparkle, frost crystal size, fern frost, snow breakup, contrast |
| Stamps | Apply Freeze Stamps |
| Performance | Generation cache and its budget |
| Preset | Preset, Preset Folder |
| Filtering / Targeting | Falloff at the box faces, floor detection, movable meshes, Excluded Actors, Include Data Layers, Include Only Actors, Required Component Tags, Slot Rules, Only Slots Matched By Rules, Overlap Policy (skip or take over meshes another zone owns) |

Selecting a zone draws its melt sources (with their drip cones or traced paths), heat sources and thaw reach, the
ground frost band and the snowfall direction in the viewport.

### Surfaces and classification

Each material slot is classified as **Stone** (the default for anything ordinary), **Metal** or **Glass**, in this
order:

1. a slot rule;
2. the tags `Icify.Stone` / `Icify.Metal` / `Icify.Glass` on the component or actor;
3. the physical material, through **Physical Surface Map** in Project Settings;
4. keywords in the material, mesh and actor names (`glass`, `window`, `metal`, `steel`, `pipe`, `brick`...);
5. the fallback type.

The surface profile scales each layer: metal frosts and glazes sooner, and glass sheds snow and grows fern frost. The
report's Slot Details say what each slot was classified as and why.

**Slot Rules** match a slot by actor, name pattern, material, component name or slot index. A rule can:

- exclude the slot;
- force its surface;
- give it its own profile, or a named one;
- force icicles on or off (**Icicles: Grow / Never**), which overrides the `Icify.NoIcicles` tag;
- gate the ice by facing, by height, or by a painted texture.

### Melt and heat sources

- **Melt sources**: hand-placed **Melt Sources** on the zone, actors in **Drip Source Actors**, and (with **Auto
  Detect Drip Sources**) anything tagged `Icify.Drip` or named like a gutter, downpipe or spout. Meltwater runs down
  from each into frozen drips and a puddle of black ice below. With **Trace Meltwater** it follows the real geometry:
  down walls, along ledges, over edges and into puddles. Drip edges on a melt-source mesh are always fed.
- **Heat sources**: actors tagged `Icify.Heat`, actors named like a fire, heater, chimney or bonfire, **Heat Source
  Actors**, and optionally point and spot lights. Everything within **Thaw Radius** melts, and further above than below,
  because heat rises. A ring of refrozen glaze forms around the thawed patch.

### Freeze stamps

**Icify Freeze Stamp** actors are hand-placed touch-ups: a sphere (scale it into an ellipsoid) that adds or erases
frost, snow, glaze, black ice and icicles inside it, with a soft noisy edge. Live-previewing zones refresh when a stamp
moves.

---

## Materials

- **MF_IcifyFrost**: the material function. Base Color, Roughness, Metallic and Normal in; the same four out, with the
  ice applied when, and only when, a freeze zone owns the mesh.
- **Hook Up Materials In Level**, **Add Icify to Selected Materials** and right-click **Add Icify** on a material or
  material instance: these wire the function into existing master materials for you. Legacy pins and Material
  Attributes are supported. Each material is backed up to `/Game/Icify/Backups` first, the change is one undo step,
  and **Remove Icify** puts the original wiring back. It refuses if you've since built on top of Icify's nodes.
  Substrate and landscape materials aren't supported yet and are skipped with a note. If the material already uses
  Grungify, Icify goes after it, so the ice sits on top of the weathering.
- **M_IcifyIcicle**: the icicle material: opaque Subsurface shading. Parameters: `Icify Ice Tint`,
  `Icify Frost Color`, `Icify Icicle Clarity` (milky to glass-clear), `Icify Icicle Ripples` and
  `Icify Icicle Ripple Size`. Set a zone's **Icicle Material** to use your own; it can read the same vertex colour and UVs.
- **M_IcifyMaskDebug**: shows the masks (Preview Display: All Masks, or one layer).
- **M_IcifyExample**: a simple material using the function, for testing.

The zone's **Palette** overrides the function's parameters on its own instances only, so two zones can give the same
master material different ice. The parameters are `Icify Frost Color`, `Icify Snow Color`, `Icify Ice Tint`,
`Icify Strength`, the four layer strengths, `Icify Relief Strength`, `Icify Sparkle`, `Icify Frost Crystal Size`,
`Icify Fern Frost`, `Icify Snow Breakup` and `Icify Contrast`.

Icify reads custom primitive data **8-12** by default (Grungify uses 0-4). It never overwrites custom data that
something else already uses on a mesh: such meshes are skipped and reported. Change **Custom Primitive Data Index** or
**Mask UV Channel** in **Project Settings > Plugins > Icify** if your project needs those slots; the content rebuilds
itself, and existing zones should then be cleared and applied again.

---

## Presets

**Tools > Icify > Create Preset Library** creates starter looks in `/Game/Icify/Library`. Each is a preset you can
assign to a zone's **Preset** (or apply with **Load Preset**):

| Preset | Look |
|---|---|
| `IP_LightFrost` | a clear, still, freezing morning: hoarfrost and fern frost, a dusting of snow, no icicles |
| `IP_DeepWinter` | weeks of snow: deep cover, drifts, long clumped icicles along the eaves |
| `IP_IceStorm` | freezing rain: thick clear glaze and dense short icicles on every drip edge |
| `IP_Blizzard` | hard wind: plastered windward walls, heavy rime, icicles leaning downwind |
| `IP_Thaw` | melting by day, refreezing by night: patchy snow, wet glaze, long thin icicles, wide black-ice puddles |

It also creates `MI_Icify_Stone`, `_Metal` and `_Glass`, instances of the example material, to try them on.
**Save To Preset** stores a zone's look (everything but scene-specific settings) as a new preset.

---

## Undo, saving and World Partition

- Preview, Apply and Clear are undoable, together with every mesh change and the icicles.
- Previews are never saved. Saving a level discards them first (autosaves keep them) and tells you.
- When World Partition unloads a zone, its preview is put back.
- Every mesh the zone touches keeps an exact backup: vertex colours, mesh-paint cache, custom primitive data and the
  raw override-material array, including its length and null entries. Clear restores all of it. A backup is never
  thrown away when restoring fails.
- A duplicated or pasted zone starts fresh: the meshes and the icicles stay with the zone that froze them.
- Two zones on one mesh: by default the second skips it (the report says so). With **Overlap Policy: Take Over**, its
  Apply takes ownership and the original backup.

## Reports

Every run fills **Last Report** on the zone and, with **Write Report Files**, writes
`Saved/Icify/Reports/<Zone>_<Operation>.json`. It covers:

- which meshes were frozen, skipped or failed, and why;
- per slot: how it was classified, which rule matched, and which gates applied;
- coverage per layer, in square metres and as a fraction;
- mask UV checks;
- drip points and icicles per mesh;
- why candidate icicles were rejected (no meltwater, thawed, thinned, no room, erased, over budget);
- the time spent in each phase.

## Scripting

`UIcifyBlueprintLibrary` (any build) and `UIcifyEditorLibrary` (editor) expose zone runs, reports, spawning, the
content build, the preset library and Freeze Actors to Blueprint and Python:

```python
import unreal
world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
for report in unreal.IcifyBlueprintLibrary.run_all_zones(world, unreal.IcifyOperation.DRY_RUN):
    unreal.log("{}: {} meshes, {} icicles".format(report.zone, report.frozen, report.icicle_stats.icicles))
```

## Performance

- Generation runs in the editor only. At runtime, the frozen meshes are ordinary meshes with vertex colours or a small
  texture, and the icicles are ordinary procedural mesh sections.
- **Max Icicles** (1500) caps each zone. A typical icicle is 100-250 vertices, built into sections of up to 60,000
  vertices each.
- Per-mesh results are cached and reused when nothing that affects a mesh changed, so tweaking icicle settings
  doesn't regenerate the masks.
- Long runs show a progress dialog with Cancel. Cancelling never leaves anything half-written.

---

## Compatibility

Unreal Engine 5.5 to 5.8; Windows, Mac and Linux editors. Icify depends on the engine's **Procedural Mesh Component**
plugin, which is enabled automatically.

### Not in 0.1 (roadmap)

- generation in the background;
- instanced meshes and foliage, skeletal meshes, spline meshes;
- landscape snow;
- Substrate material hookup;
- Nanite displacement for snow depth;
- baking ice and icicles into standalone assets;
- runtime freeze and thaw;
- a zones panel and a commandlet.

---

## Development

### First compile

1. Put the plugin in a C++ project on UE 5.8 (or run `Scripts/BuildPlugin.ps1 -EngineVersions 5.8` /
   `Scripts/BuildPlugin.sh --versions 5.8 --skip-readiness`). Fix anything the compiler names. The likely spots are
   listed in `Docs/EngineCompatibility.md`.
2. Open the editor and check `Saved/Icify/ContentReport.txt` says **Ready**.
3. Run the automation tests (**Tools > Test Automation**, filter `Icify`).
4. Freeze something with eaves and ledges, and look at it.

### Offline checks (no engine needed)

| Script | What it checks |
|---|---|
| `python3 Scripts/CheckShaders.py` | compiles `Icify.ush` and every custom node the content builder generates (read straight out of `IcifyContentBuilder.cpp`) with glslang, pixel and vertex stages, plus HLSL 2021 lints. Needs `glslang-tools`. |
| `python3 Scripts/CheckIcicles.py` | builds the real icicle builder against a small CoreMinimal shim and runs 45 geometry checks: closed surfaces, Unreal winding, outward normals, tip length, the root cap, determinism. `--obj out.obj` writes a sample cluster. |
| `python3 Scripts/CheckFabReadiness.py` | copyright headers, descriptor, prebuilt content, icon, source hygiene, Build.cs settings. |

CI (`.github/workflows/checks.yml`) runs all three on every push.

### Automation tests

| Test | Covers |
|---|---|
| `Icify.Generation.LayerRules` | each layer forms where it should (sky cover, slopes, floors, wind, thaw) |
| `Icify.Generation.SkyCoverLookup` | the sky shadow map: covered, open, faces turned away |
| `Icify.Generation.DripEdges` | drip edges on ledges, a pipe's lowest line, outward directions, determinism, icicles off |
| `Icify.State.*` | exact custom data, paint cache and material-array restoration, overlap ownership, owned material instances, World Partition unload |
| `Icify.Icicles.PreviewApplyClearRoundTrip` | icicles through the real zone: preview, apply, preview over apply, cancel, clear, determinism, roof cover, the NoIcicles tag |
| `Icify.Icicles.SkyCoverFirstHit` | sky traces in a real world: first hit, actor and height |
| `Icify.Content.InPlaceRegeneration` | rebuilding the generated content in place for different settings (run in the BuildPlugin HostProject) |
| `Icify.Content.MaterialHookup*` | hookup round trip with undo and redo, refusals, preserving artist edits |

### Layout

```
Icify.uplugin
Config/FilterPlugin.ini
Resources/Icon128.png
Shaders/Private/Icify.ush                     shading for the masks, the mask debug view and the icicles
Source/IcifyShaders/                          maps /Plugin/Icify to Shaders/ (PostConfigInit)
Source/Icify/                                 runtime: zone, stamp, preset, settings, generator, icicle builder, scripting
Source/IcifyEditor/                           content builder, material hookup, quick start, preset library, visualizer, menus
Scripts/                                      offline checks and packaging
Docs/EngineCompatibility.md                   engine API notes for 5.5-5.8
```
