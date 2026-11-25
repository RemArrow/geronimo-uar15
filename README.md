# UAR-15 for Geronimo

A [GML](https://github.com/RemArrow/geronimo-mod-loader) plugin that adds a **UAR-15** to the gun station's
**Assault Rifles** list: an AR-15 with a 20" barrel, a long free-float handguard with M-LOK slots, Magpul MBUS
sights and a BCM stock. It replaces no game asset.

The model and textures are the UAR-15 from the BONELAB *Cold Weapon Pack* by Bonewar (itself built from
third-party models), **redistributed with permission**. They ship encrypted inside `UAR15.dll` in the release zip.
The source art stays out of this repository (`Assets\` is gitignored).

## Install

Needs [GML 2.4 or newer](https://github.com/RemArrow/geronimo-mod-loader/releases). Install it from GML Manager's
Browse tab, or close the game, download `UAR15-<version>.zip` from
[Releases](https://github.com/RemArrow/geronimo-uar15/releases) and extract it into
`...\GERONIMO\Geronimo\Binaries\Win64`. That adds `GML\plugins\UAR15`.

## How it works

- **Base gun: the game's M4A1 Block II** (`DA_M4A1_BlockII`, "M4A1 CQBR"). It has the same AR-15 action, 5.56
  round, STANAG magazine, charging handle and selector. Handling, grips, magazines (the game's 30-round PMAG),
  rounds, sound and recoil all stay the M4's.
- **Fit:** both guns are mil-spec AR-15s, so `tools\prep` fits the UAR-15's upper and lower receivers onto the
  cooked M4 mesh. It starts with the chambers together, then runs a trimmed ICP (translation, then rigid):
  **2.9 mm rms**, 0.26° of turn. The bores line up within 0.5 mm. `build\preview\fit_*.png` shows the M4's
  points (cyan) over the UAR-15.
- **Moved points:** the muzzle (`Fire Location`, the shot origin and muzzle-device mount) goes from 34.4 to
  50.5 cm, the rear of the UAR-15's A2 flash hider. The stock butt goes from −36.2 to −30.8. Everything else
  (chamber, magazine well, charging-handle grab box, hand sockets) already sits on the UAR-15's parts.
- **Its own moving parts:** each follows one of the M4's movables bones every frame, turned about the UAR-15's
  own pivot (the BONELAB rig's `TP` / `SP` nodes):

  | Part | Follows | Verified in game |
  |---|---|---|
  | Bolt carrier | `RootBoltCarrierGroup` (slides) | 4.9 cm at half pull, 9.75 at full |
  | Charging handle | `RootChargingHandle` (slides) | with the carrier |
  | Magazine release | `RootMagRelease` (slides) | pushes in 3 mm |
  | Trigger | `RootTrigger` (turns) | 7° back on its pin |
  | Selector | `RootSelector` (turns) | safe 0°, semi 90°, auto 180° |

- **Flash hider** hides while a muzzle device is mounted. **MBUS sights** hide under an optic on the top rail.
- **Multiplayer and loadouts:** the same scheme as the Barrett and the Saiga. The receiver's `ColorOption` is
  set to `UAR15` (replicated, and kept in a saved loadout). A loadout slot holding the UAR-15 is re-saved with
  the M4 donor and recognised by that colour. Verified: save, `LoadGunSetups`, and it comes back as a UAR-15.

## Build from source

The art is not in this repository, so building needs your own export of the Cold Weapon Pack's UAR-15.

```
build.bat prep      Assets\ from Work\BonelabPort\Guns\UAR-15\UAR-15.glb (set UAR_GLB to use another)
build.bat           build\UAR15\UAR15.dll and the test harness
build.bat install   also copy the plugin into the game
```

`tests\run_test.ps1` runs `tests\UAR15Dev` in the installed game, flat in a window. It snapshots and restores
SaveGames, the GML configs, the plugins folder and `doorstop_config.ini` (hash-verified), and switches GML on for
the run if the GML Manager has it off.

## Not done yet

- Not tried in VR or in a two-player session.
- The model has colour maps only. Normals are flat, and roughness comes from the colour map's brightness.

## Loadout fix (0.1.3)

The game saves a stock M4A1 CQBR under the UAR-15's gun entry, because both guns share one receiver. 0.1.2 took
that as a UAR-15, so a CQBR in your loadout spawned as a UAR-15 with the CQBR's attachments. Now only the UAR-15's
colour name, which every UAR-15 carries into its save, marks a UAR-15 slot. A CQBR slot that 0.1.2 had taken over
loads as a CQBR again. Verified in game: UAR-15 saved and reloaded as a UAR-15, then a CQBR saved over it and
reloaded as a CQBR.

## Textures (0.1.1)

The colour maps come from Arma and average only 0.6-1.9% linear brightness: near black in the game's lighting, made
worse by the first build marking most parts fully metallic (a dark metallic surface reflects almost nothing).
`tools\prep` now brings any map under 5% up to 5% (at most 6x, keeping detail and hue). Metallic is set only on the
bare steel, the bolt and charging handle (0.7); anodized aluminium, parkerized steel and polymer are dielectrics.
Result: 3.4-5% for every part, a dark grey rifle rather than a black silhouette. The M16A2 gets the same.
