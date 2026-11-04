# Human rig cheat sheet (MEASURED, not assumed)

Everything below was measured empirically in the engine: one channel at a time, rendered,
and the render was looked at. Nothing here is inferred from Minecraft/Blockbench habits.
Evidence: `evid/rig/sheets/*.png` (comparison sheets), `evid/rig/ops/p*.json` (op-scripts),
`evid/rig/shots/*.png` (raw shots). 10 headless runs, 5032 ops, 5032 ok, 368 rendered shots.

---

## 1. Rig anatomy

`mi3_object_add {kind: "character", body: "classic"|"slim", variant: steve|alex|zuri|sunny|noor|makena|kai|efe|ari}`
builds the catalogue `human` model. (Internally this is the engine's `object.add_character`;
every measurement below was taken through it, so the numbers transfer exactly.)

```
<Character>            pivot (  0, 0,  0)   type CHARACTER    <- the root you move/place
+- Body                pivot (  0, 0, 12)   parent = Character
|  +- Head             pivot (  0, 0, 24)   parent = Body
|  |  +- Hat           pivot (  0, 0, 24)   parent = Head
|  +- Right arm        pivot ( -6, 0, 22)   parent = Body
|  +- Left arm         pivot ( +6, 0, 22)   parent = Body
+- Right leg           pivot ( -2, 0, 12)   parent = Character   <-- NOT under Body
+- Left leg            pivot ( +2, 0, 12)   parent = Character   <-- NOT under Body
```

`object.list` (engine op, not a tool) returns 8 rows (root + 7 parts). Pivots are at the TOP of each limb box.

**Units.** 16 units = 1 Minecraft block. Character is 32 tall (feet Z=0, top of head Z=32).
Arms and legs are 12 long. `slim` and `classic` have IDENTICAL hierarchy and pivots
(only the arm box is 3 wide instead of 4), so every angle in this document transfers.

**Two consequences that bite:**

1. **The legs are siblings of Body, not children.** Rotating `body` leans/twists the torso,
   head and arms, and the legs STAY STANDING. Verified visually. To turn or lean the whole
   figure, rotate the ROOT (the character object itself), not `body`.
2. **Head is a child of Body**, so head angles ADD to body angles. If body ROT_Z = -6 and
   head ROT_Z = +5, the head ends up at -1 in world terms.

**Refs.** `"<Character>/right_arm"`. Resolution is by save_id, exact display_name, or
NORMALISED name (lowercase, `_` treated as space), so `right_arm` matches `"Right arm"`.
Hierarchical paths use `/` (`mi3_hl_resolve` / `mi3_hl_name_norm`).

---

## 2. World frame and which side is which

- **+Z is up.** The ground plane is XY.
- **A freshly spawned character FACES +Y.** (Measured: a camera at +Y sees the face,
  a camera at -Y sees the back of the head.)
- **The character's OWN RIGHT is -X. Its OWN LEFT is +X.** `Right arm` sits at X = -6.
  This is consistent, not a bug: from a camera in +Y (where screen-right = +X) the
  Right arm appears on the VIEWER's left, exactly as when you face a real person.

Say "the character's right" / "screen right" explicitly. Never just "right".

---

## 3. THE ONE RULE

Measured on all 6 parts and all 3 channels, no exceptions:

> **`ROT_A(+theta)` is a right-handed rotation of MINUS theta about the `+A` axis.**
> (GameMaker's left-handed positive sense.)

Operationally, for any part:

| channel | positive value moves the part's UPPER end | ...and its LOWER end |
|---|---|---|
| `ROT_X` | toward **+Y (forward)** | toward -Y (backward) |
| `ROT_Y` | toward **-X (character's RIGHT)** | toward +X (character's left) |
| `ROT_Z` | yaws the facing (+Y) toward **+X (character's LEFT)** | same |

Arms and legs point DOWN, torso and head point UP. That is why the same channel with the
same sign reads OPPOSITE on a limb and on the torso. This is the single biggest trap in
this rig; the table in section 4 spells it out so you never have to re-derive it.

---

## 4. Per-part semantics (measured at +/-30, three views each)

| part | `ROT_X` + | `ROT_X` - | `ROT_Y` + | `ROT_Y` - | `ROT_Z` + | `ROT_Z` - |
|---|---|---|---|---|---|---|
| `right_arm` | swings **BACK** (extension) | swings **FORWARD** (flexion) | **ADduct** (in, across chest) | **ABduct** (out to the side) | twist **inward** (internal rot.) | twist outward |
| `left_arm` | **BACK** | **FORWARD** | **ABduct** (out) | **ADduct** (in) | twist outward | twist inward |
| `right_leg` | **BACK** | **FORWARD** | **ADduct** (crosses midline) | **ABduct** (splay out) | toe turns **IN** | toe turns OUT |
| `left_leg` | **BACK** | **FORWARD** | **ABduct** (splay out) | **ADduct** (crosses) | toe turns OUT | toe turns IN |
| `body` | lean **FORWARD** (bow) | lean **BACK** | tilt toward character's **RIGHT** | tilt toward LEFT | twist shoulders toward character's **LEFT** | toward RIGHT |
| `head` | look **DOWN** (nod) | look **UP** | tilt ear toward **RIGHT** shoulder | toward LEFT shoulder | turn toward character's **LEFT** | toward RIGHT |
| `hat` | follows Head. Do not pose it. | | | | | |

### Mirroring left <-> right

The left and right parts use the SAME world-aligned axes; they are NOT anatomically
mirrored. To mirror a pose:

```
keep    ROT_X
NEGATE  ROT_Y, ROT_Z
swap    right_arm <-> left_arm,  right_leg <-> left_leg
body/head: negate ROT_Y and ROT_Z, keep ROT_X
```

This rule was **validated end to end**: applying it to `pose_walk` (section 8.2) produced
exactly the opposite key frame of the same walk -- same facing, same stride direction,
limbs swapped. See `evid/rig/sheets/val_mirror.png`.

> **WARNING.** `object.mirror_pose` (engine op, not a tool) does **NOT** do this. It copies ROT_X/Y/Z
> verbatim from one object to another (source: `mi3_headless_ops2.gml`, `mi3_hl_ext_mirror_pose`).
> Its real purpose is player -> armour pose transfer. It is a COPY, not a mirror.
> Do the mirroring yourself with the rule above.

---

## 5. Euler composition (order matters, write order does not)

Engine order applied to a column vector is `RotZ(rz) * RotX(rx) * RotY(ry)`:
**Y first (innermost), then X, then Z (outermost).**

Verified: setting ROT_Y then ROT_X produces a pixel-identical render to setting ROT_X then
ROT_Y. **The order of your `mi3_set_values` calls is irrelevant** -- only the engine's fixed
Euler order matters (measured through `value.set`, engine op, not a tool).

How to think about it on a limb:

- `ROT_Y` is innermost: on a limb hanging straight down it is a clean side-swing (abduction).
- `ROT_X` then rotates that ALREADY-abducted configuration about the **world** X axis. An
  abducted arm rotated in X ends up out **and up-forward**, not on a simple 45-degree diagonal.
- `ROT_Z` is outermost: a **world-vertical yaw about the part's pivot**. On a limb hanging
  straight down it looks like a pure twist. On a RAISED limb it sweeps the limb horizontally.

**Measured critical case (shoulder, flexion + abduction):**

| set | result |
|---|---|
| `right_arm ROT_X = -60` | arm forward and down 60 deg |
| `right_arm ROT_Y = -60` | arm out to the side, down 30 deg from horizontal |
| both together | arm out, forward AND above the shoulder line -- *not* the naive halfway diagonal |
| plus `ROT_Z = +45` | the whole thing sweeps further forward |

**Practical recipe:** to raise an arm sideways and then bring it forward, use
`ROT_Y` for the raise and `ROT_Z` for the forward sweep -- **not** `ROT_X`. Using ROT_X on
an already-raised arm swings the hand across the face. (This is exactly why the first
attempt at `pose_wave` failed; see section 8.)

For a right-side raised arm, `ROT_Z` **positive** brings it FORWARD (+Y).

---

## 6. BEND

`BEND_ANGLE_X / _Y / _Z`, clamped to +/-180 (`tl_value_clamp.gml`). Measured behaviour on
the human rig:

**Limbs (`right_arm`, `left_arm`, `right_leg`, `left_leg`):**

- Only `BEND_ANGLE_X` does anything. `BEND_ANGLE_Y` and `BEND_ANGLE_Z` at +/-60: no effect.
- **Positive only.** Negative values are a silent no-op. Verified at -30, -60, -90, -150
  and -179: the limb stays perfectly straight.
- The direction is anatomically pre-baked and you cannot reverse it:
  **arms fold the elbow FORWARD (+Y), legs fold the knee BACKWARD (-Y).** Both correct.
- Useful range: 10-20 subtle, 30-60 clearly bent, 80-90 tight fold (sitting), >120 folds
  the limb onto itself.

**`body`:** all three axes work and **both signs work**.

- `BEND_ANGLE_X` +: spine curves forward. -: curves backward (arched chest). Same sense as ROT_X.
- `BEND_ANGLE_Y` +: curves toward the character's **LEFT**. *(Note the trap: this is the
  OPPOSITE sense to `body ROT_Y`, which tilts toward the RIGHT for positive.)*
- `BEND_ANGLE_Z` +: twist-curve toward the character's LEFT (same sense as ROT_Z).

**`head` / `hat`:** no bend at all (it is a single cube). `BEND_ANGLE_X = 45` changed nothing.

---

## 7. Aesthetic limits (where it starts to read broken)

Judged on clean ortho renders, no ground, no shadow -- i.e. the harshest possible reading.
In a real shot with ground contact and lighting these are more forgiving.

| joint / channel | comfortable | strong but fine | breaks |
|---|---|---|---|
| arm `ROT_X` (fore/aft) | 0 .. +/-45 | +/-90 (horizontal) | never tears; at -150..-180 the arm grazes the head. Full +/-180 is usable. |
| arm `ROT_Y` (abduction) | 0 .. -60 | -90 (T-pose arm) | -120..-150: a notch opens between sleeve and shoulder. Stylised only. |
| arm `ROT_Z` (twist) | +/-30 | +/-90 | >+/-90 the box corner pushes out of the sleeve; reads as a dislocated shoulder. |
| leg `ROT_X` | 0 .. +/-45 | -60 .. -90 | past -90 (or +60 backward) a clear gap opens at the hip. |
| leg `ROT_Y` (splay) | +/-8 | +/-15 (small pelvic gap on a clean front ortho) | +/-60..90: an obvious hole at the hip. |
| `body ROT_X` | +/-20 | +/-35 | **+/-50 and beyond: the waist visibly separates**, because the legs do not follow. |
| `body ROT_Z` | +/-20 | +/-35 | +/-50: shoulders detach from hips (same cause). |
| `head ROT_Z` | +/-45 | +/-60..75 | +/-90: owl turn, cube corners overhang the neck. |
| `head ROT_X` | +/-30 | +/-45 | +/-60..75: the head sinks into the chest / floats off it. |
| `head ROT_Y` | +/-20 | +/-30 | +/-45 and up: the neck gap shows. |

> **Rule of thumb.** The rig never tears on arm or head rotations (they are boxes on a
> pivot). The only two places a hole appears are the **WAIST** and the **HIP**, and both
> for the same reason: `Body` and the legs are SIBLINGS, not a chain. Keep `body ROT_X/ROT_Z`
> under ~35 unless you are also posing the legs to sell the lean.

---

## 8. Canonical poses

Every one of these was rendered and looked at. Values are absolute `value.set` (engine op, not
a tool -- use `mi3_set_values`) on the character's parts; anything not listed is 0. The root
row (`""`) is the character object itself.

### 8.1 `idle` -- relaxed standing

| part | values |
|---|---|
| `body` | `ROT_X 1, ROT_Y 3, ROT_Z -4` |
| `head` | `ROT_X 3, ROT_Y -2, ROT_Z 6` |
| `right_arm` | `ROT_X 5, ROT_Y -6, BEND_ANGLE_X 9` |
| `left_arm` | `ROT_X 3, ROT_Y 6, BEND_ANGLE_X 6` |
| `right_leg` | `ROT_X -3, ROT_Y -2` |
| `left_leg` | `ROT_X 3, ROT_Y 3` |

Reads relaxed because nothing is symmetric and the arms are 6 deg off the torso with a
small elbow. Arms flush at 0 read as a mannequin. Camera: either 3/4.

### 8.2 `walk` -- key frame (contact extreme)

| part | values |
|---|---|
| *(root)* | `POS_Z -1.2` |
| `body` | `ROT_X 5, ROT_Z -6` |
| `head` | `ROT_X 2, ROT_Z 5` |
| `right_arm` | `ROT_X 32, ROT_Y -4, BEND_ANGLE_X 10` |
| `left_arm` | `ROT_X -30, ROT_Y 4, BEND_ANGLE_X 16` |
| `right_leg` | `ROT_X -28, ROT_Y -2` |
| `left_leg` | `ROT_X 26, ROT_Y 2, BEND_ANGLE_X 14` |

Contralateral: right leg forward (`ROT_X -28`) pairs with LEFT arm forward (`ROT_X -30`).
`body ROT_Z -6` is the shoulder counter-twist against the stride; `head ROT_Z +5` cancels it
so the gaze stays on the direction of travel. Root drops 1.2 because both legs are splayed.
Mirror with the section 4 rule for the opposite key frame; the mid-frames go through
near-zero with the root ~1.2 higher. Camera: side view or either 3/4.

### 8.3 `sit` -- seated on a 12-unit surface

| part | values |
|---|---|
| `body` | `ROT_X 6` |
| `head` | `ROT_X 3` |
| `right_arm` | `ROT_X -14, ROT_Y -7, BEND_ANGLE_X 20` |
| `left_arm` | `ROT_X -14, ROT_Y 7, BEND_ANGLE_X 20` |
| `right_leg` | `ROT_X -80, ROT_Y -6, BEND_ANGLE_X 82` |
| `left_leg` | `ROT_X -80, ROT_Y 6, BEND_ANGLE_X 82` |

Thigh horizontal forward (`ROT_X -80`, just inside the hip limit), knee folded back
(`BEND_ANGLE_X 82`) so the shin drops. **Leave root `POS_Z` at 0**: the hips then sit at
Z=12 and the feet land at Z~0, i.e. the character is correctly seated on a one-block-high
surface with the feet on the floor. Camera: 3/4 (a pure front view reads as a torso on stubs).

### 8.4 `wave`

| part | values |
|---|---|
| `body` | `ROT_Y 2, ROT_Z -6` |
| `head` | `ROT_X -2, ROT_Z -8` |
| `right_arm` | `ROT_Y -138, ROT_Z 18, BEND_ANGLE_X 28` |
| `left_arm` | `ROT_X 4, ROT_Y 6, BEND_ANGLE_X 8` |
| `right_leg` | `ROT_X -3, ROT_Y -4` |
| `left_leg` | `ROT_X 3, ROT_Y 4` |

`ROT_Y -138` raises the arm out and up past horizontal; `ROT_Z +18` sweeps it forward;
the elbow bend brings the hand up. **Do not use `ROT_X` here** -- because Z is outermost and
X is applied to the already-raised arm, `ROT_X` drags the hand across the face
(this was the failure mode of the first attempt). Camera: front-right 3/4 is the best read.

### 8.5 `point` -- pointing forward

| part | values |
|---|---|
| `body` | `ROT_X 3, ROT_Z 7` |
| `head` | `ROT_X -2, ROT_Z -7` |
| `right_arm` | `ROT_Y -30, ROT_X -88` |
| `left_arm` | `ROT_X 6, ROT_Y 7, BEND_ANGLE_X 12` |
| `right_leg` | `ROT_X -9, ROT_Y -2` |
| `left_leg` | `ROT_X 6, ROT_Y 4` |

Straight arm (no bend) pointing forward and ~30 deg to the character's right.
**Camera matters for this one:** shoot it from the **front-LEFT 3/4** (`ROT_Z 225`) or from
the character's left side. From the front-RIGHT 3/4 the arm points nearly at the lens and
collapses into a foreshortened block. Verified both ways.

### 8.6 `hero` -- power stance

| part | values |
|---|---|
| `body` | `ROT_X -5, ROT_Z -3, BEND_ANGLE_X -8` |
| `head` | `ROT_X 3, ROT_Z 5` |
| `right_arm` | `ROT_X 8, ROT_Y -22, BEND_ANGLE_X 16` |
| `left_arm` | `ROT_X 8, ROT_Y 22, BEND_ANGLE_X 16` |
| `right_leg` | `ROT_X -3, ROT_Y -11` |
| `left_leg` | `ROT_X -3, ROT_Y 11` |

`body ROT_X -5` plus `BEND_ANGLE_X -8` arches the chest out without breaking the waist;
`head ROT_X +3` cancels the inherited backward lean so the gaze stays level (otherwise the
character stares at the sky). Legs at +/-11 is the practical splay limit -- at +/-18 a
pelvic gap becomes visible on a clean front render. Camera: either 3/4.

### 8.7 `fist up` -- one arm raised

| part | values |
|---|---|
| `body` | `ROT_X -6, ROT_Z -8, BEND_ANGLE_X -10` |
| `head` | `ROT_X -4, ROT_Z -6` |
| `right_arm` | `ROT_Y -115, ROT_Z 14, BEND_ANGLE_X 34` |
| `left_arm` | `ROT_X 10, ROT_Y 16, BEND_ANGLE_X 14` |
| `right_leg` | `ROT_X -4, ROT_Y -9` |
| `left_leg` | `ROT_X -2, ROT_Y 13` |

`ROT_Y` raises (section 5: never `ROT_X` on a raised arm), `ROT_Z +14` sweeps it forward, the elbow
brings the fist up. **Do not push `ROT_Y` past about -120**: at -140 the notch between sleeve and
shoulder that section 7 warns about opens up and the arm reads detached (measured, looked at).

### 8.8 `wide guard` -- braced, arms out and bent

| part | values |
|---|---|
| *(root)* | `POS_Z -1.5` |
| `body` | `ROT_X 4, ROT_Z 6, BEND_ANGLE_X 6` |
| `head` | `ROT_X -3, ROT_Z -8` |
| `right_arm` | `ROT_X -18, ROT_Y -46, BEND_ANGLE_X 34` |
| `left_arm` | `ROT_X -14, ROT_Y 42, BEND_ANGLE_X 30` |
| `right_leg` | `ROT_X -6, ROT_Y -14` |
| `left_leg` | `ROT_X -6, ROT_Y 14` |

Root drops 1.5 because both legs are splayed (same reason as `walk`).

### 8.9 `crouch` -- coiled, lower head line

| part | values |
|---|---|
| *(root)* | `POS_Z -2.81` |
| `body` | `ROT_X 10, ROT_Z -5, BEND_ANGLE_X -4` |
| `head` | `ROT_X -8, ROT_Z 6` |
| `right_arm` | `ROT_X -22, ROT_Y -30, BEND_ANGLE_X 40` |
| `left_arm` | `ROT_X -6, ROT_Y 24, BEND_ANGLE_X 20` |
| `right_leg` | `ROT_X -40, ROT_Y -8, BEND_ANGLE_X 80` |
| `left_leg` | `ROT_X -40, ROT_Y 8, BEND_ANGLE_X 80` |

The root offset is not decoration: it is `6*cos(40) + 6*cos(40) - 12 = -2.81` (12.4). Verified with
`state.dump`: the leg pivot lands at world_z 9.19, exactly where the formula says, and the feet sit
on the ground. For a deeper crouch use thigh 55 / bend 100 and root -4.32 (also verified).

### 8.10 `kneel` -- one knee down, torso UPRIGHT (round L)

| part | values |
|---|---|
| *(root)* | `POS_Z -5.48` |
| `body` | `ROT_X 3, ROT_Z -6` |
| `head` | `ROT_X -6, ROT_Z 8` |
| `right_arm` | `ROT_X -30, ROT_Y -30, BEND_ANGLE_X 35` |
| `left_arm` | `ROT_X -8, ROT_Y 22, BEND_ANGLE_X 18` |
| `right_leg` (down) | `ROT_X 0, ROT_Y -6, BEND_ANGLE_X 85` |
| `left_leg` (front) | `ROT_X -85, ROT_Y 6, BEND_ANGLE_X 85` |

Hip: `6*cos(0) + 6*cos(85) = 6.52` (12.4's formula), root `6.52-12 = -5.48`: down knee grazes the
ground, front foot flat, nothing floats. **The low pose for a group** -- reads LOWER than `crouch`
(head 5.5 down vs 2.8) but keeps the torso vertical, collecting the key like a standing body:
`crouch` +32% vs `kneel` +41% at key elevation 35, +47% vs +53% at 20 (round L). Same
`right_arm ROT_Y -30` as `crouch`, so 12.4's read-back is unchanged.

---

## 9. Camera conventions (calibrated in this run)

- The camera looks along its **local +Y** (`camera.focus_on`, engine op, not a tool, uses
  `point3D(0,1,0)`).
- **`ROT_Z`:** 0 -> looks +Y; 90 -> +X; 180 -> -Y; 270 -> -X.
  World look direction = `(sin ROT_Z, cos ROT_Z, ...)`.
- **Aim at a target:** `ROT_Z = degrees(atan2(tx - px, ty - py))` -- note the argument order,
  X first.
- **`ROT_X`: POSITIVE pitches DOWN**, negative pitches UP. `ROT_X = 90` is straight down but
  sits on a gimbal with ROT_Z (the screen axes flip unpredictably) -- prefer <= 75 and place
  the camera higher and further back instead.
- **Screen mapping:** `screen_right = cross(up, forward)`. So looking along -Y (front view of
  the character) screen-right = +X; looking along +Y (back view) screen-right = -X.

Because the character faces +Y, with a character at the origin and D = distance:

| shot | position | `ROT_Z` | `ROT_X` |
|---|---|---|---|
| FRONT | `(0, +D, 17)` | 180 | 0 |
| BACK | `(0, -D, 17)` | 0 | 0 |
| character's RIGHT side | `(-D, 0, 17)` | 90 | 0 |
| character's LEFT side | `(+D, 0, 17)` | 270 | 0 |
| **3/4 front-right (default flattering angle)** | `(-0.707D, +0.707D, 26)` | **135** | +8 |
| 3/4 front-left | `(+0.707D, +0.707D, 26)` | **225** | +8 |
| steep top-front (no gimbal) | `(0, +0.4D, +D)` | 180 | +62 |

- **`CAM_FOV` is the VERTICAL fov in degrees.** Default 45, clamped 1..170. 40 is a good
  character lens; 70 is the wide default used by the editor's own viewport.
  Full-body framing of the 32-unit character at FOV 40 needs D ~ 63
  (vertical extent = `2 * D * tan(FOV/2)` ~= 46).
- **`CAM_ORTHO = 1` + `CAM_ORTHO_SCALE`** = the view WIDTH in world units (default 128);
  `CAM_FOV` is ignored. ~42-46 gives a full-body 1:1 frame with the camera at Z 17.
  Ortho is the right tool for comparison/measurement sheets.
  **Measured caveat:** in ortho the engine does not clip behind the camera plane, so a
  camera whose look direction points AWAY from the subject still renders it (seen from the
  opposite side, with reversed depth ordering). Always aim the camera properly even in ortho.
- **Depth of field:** `CAM_DOF` on/off, `CAM_DOF_DEPTH` = focus distance in world units.
  The engine op `camera.focus_on {ref, target}` (not a tool) sets it from the PLANAR distance
  along the optical axis -- you do this with `mi3_set_values` on `CAM_DOF_DEPTH` directly.
  Defaults: `CAM_DOF_RANGE` 200, `CAM_DOF_BLUR_SIZE` 0.01.
  The f-stop conversion the UI uses (`action_tl_frame_cam_dof_fstop.gml`):
  `blur = 0.003 / (tan^2(FOV/2) * N * focus)`, clamped 0..0.2, with N >= 0.05.

---

## 10. Working gotchas

- **Warmup shot.** The first preview after building or altering a scene can render an
  unsettled pose (measured through the engine op `camera.preview`, not a tool -- your
  equivalent is `mi3_camera_preview`). Throw away one small preview before the real shot.
  Every sheet backing this document was produced that way.
- **Keyframes beat `value.set`** (engine op, not a tool -- `mi3_set_values` is the equivalent).
  If the part already has keyframes, use `keyframe.add` (engine op, not a tool -- you have
  `mi3_keyframe_add`); plain `value.set` will be overridden on the next animate pass.
- **Verify numerically.** `state.dump {out}` (engine op, not a tool -- use `mi3_object_get`
  per-object instead) writes `world_x/y/z` for every part; that is how poses here were checked
  instead of eyeballed, especially for contact (feet on the floor).
- **Do not pose `Hat`.** It rides on Head.
- Renders are cheap: `camera.preview` (engine op, not a tool) at 512x512 samples 1 costs
  ~30 ms; a headless run's real cost is the ~15 s process start.

---

## 11. Confidence

**Solid (measured directly, multiple angles, looked at):** hierarchy and pivots; the
character faces +Y and its right is -X; the one rotation rule; the per-part ROT_X/Y/Z table
for all six parts; left/right are not mirrored and `mirror_pose` does not mirror; the Euler
composition order and the shoulder critical case; BEND is positive-only on limbs with a
pre-baked direction; the camera conventions; the six poses.

**Weaker:** `body BEND_ANGLE_Z` sign (one +/-45 pair, read from a single steep view);
the exact break-angle thresholds in section 7 are eyeball judgements on ground-less,
shadow-less ortho renders and will be more forgiving in a real shot; slim was only verified
at neutral (pivots identical to classic, so angles should transfer, but the poses were
tuned on classic).

**Not probed:** `IK_TARGET / IK_BLEND / IK_TARGET_ANGLE / IK_ANGLE_OFFSET`; `POS_*` and
`SCA_*` on individual parts; how rotations interpolate across the +/-180 wrap in keyframes;
armour parts (see the per-part attachment note in project memory).

---

## 12. Group staging (MEASURED, round G)

The mistake to not make: lining up N characters on X with the same Y. With any perspective
camera that produces a vanishing-line effect: measured with 4 characters at X = -30/-11/+11/+30,
Y = 0 and a 3/4 camera, their on-screen heights came out 122.0 / 88.9 / 68.7 / 58.2 percent of
the frame (the nearest one takes up 2.10x what the farthest one does), and the nearest one
clipped out of frame.

### 12.1 The equidistant arc (the formula)

Place the N subjects on an arc CENTERED ON THE CAMERA. That way everyone is the same distance
from the lens and comes out the same size on screen. Gives 1.064 size spread instead of 2.10
(measured).

**Order matters: decide `C` and `phi` BEFORE you compute a single `P_i`** (every `P_i` is defined
relative to the camera's position and angle). Decide where the camera sits and looks first
(created before or after the characters, doesn't matter), THEN compute `P_i` for every subject
from THAT `C`/`phi`. Moving the camera later makes the old `P_i` values stale -- recompute them,
or move the subjects to match.

Given: camera at `C = (cx, cy)`, look azimuth `phi` = camera `ROT_Z`, base distance `D` to the
group's center.

1. **Angular step.** Apparent width of one character: `w = 2 * atan(7 / D)` degrees (7 = usable
   half-width with the arms out in a hero pose). Spacing between neighbours: `dTheta = 2.0 * w`.
   **1.35 was measured to be too tight**: at that spacing consecutive bounding boxes overlap by
   11-14 points of frame width and the arms of neighbours touch, which fuses four figures into one
   blob. At 2.05 the overlap is ~0.
2. **Each one's angle.** `theta_i = (i - (N-1)/2) * dTheta`, `i = 0..N-1`.
3. **Staggered radius** (breaks the perfectly flat arc without breaking the scale):
   `r_i = D * (1 + 0.05 * s_i)` with `s_i = +1` for the outer two and `-1` for the centre ones
   (a cradle toward the camera: reads as a spearhead). 5% of radius is only a 10% size difference.
   Do not push it past 5%: measured, the wide-angle stretch on the outer subjects already costs
   you 1.07-1.14 of size spread on its own.
4. **Position.** `P_i = C + r_i * (sin(phi + theta_i), cos(phi + theta_i))`. Watch the order: X
   uses sin, Y uses cos (same convention as the camera, section 9).
5. **Yaw: ONE yaw for the whole group, not one per subject.** Let `C_mid` be the azimuth of the
   camera as seen from the CENTRE of the group -- with the arc centred on the camera that is just
   `camera ROT_Z + 180`, wrapped into -180..180. Give **every** subject the same
   `ROT_Z = C_mid + 30 * s`, and pick `s` (+1 or -1) so the chests turn toward the side the key
   light comes from (13.3). Do NOT give each subject its own `C_i + 30`: that fans the chests out
   by up to 47 degrees across the group, and since the key sits at ONE azimuth every subject then
   meets it at a different angle. Measured, same scene, same light, only the yaw policy changed:
   per-subject gain ratio **1.26 -> 1.12** and the worst subject's lateral read **1.30 -> 1.75**.
   Parallel chests also make the check trivial: all N `ROT_Z` values must be the same number.
   (The 25-40 band still applies to the offset itself: at `C_mid + 15` the camera sees essentially
   one face and no lighting can put a gradient on it -- measured, lateral read collapses to 1.08.)

**Verified example (round H, N = 4, D = 105, phi = 180, camera (0, +105, 20), FOV 40, 16:9):**
`w = 7.63`, `dTheta = 15.6`, `aim_z = 14` -> `ROT_X = +3.3`.

| i | theta | r | POS_X | POS_Y | C_i | ROT_Z (C_mid + 30, igual para todos) |
|---|---|---|---|---|---|---|
| 1 | -23.4 | 110.3 |  43.89 |  3.86 | -23.46 |   30.00 |
| 2 |  -7.8 |  99.8 |  13.57 |  6.18 |  -7.82 |   30.00 |
| 3 |  +7.8 |  99.8 | -13.57 |  6.18 |  +7.82 |   30.00 |
| 4 | +23.4 | 110.3 | -43.89 |  3.86 | +23.46 |   30.00 |

the four `ROT_Z` are the same number on purpose -- see point 5.

Measured with `mi3_screen_layout` at 960x540: size spread **1.137**, `abs(mean(center_pct_x) - 50)
= 0.7`, spacing 22.9 / 20.5 / 24.4 points, every bbox inside `[4.2%, 94.7%]`, feet at 70.8-77.2%.
Rendered: `h11_A_full.png`.

**Distance is what buys you the spacing.** The same `dTheta = 2.05 * w` at `D = 90` puts the outer
two subjects' boxes at `-7.2%` and `105.0%` and cuts their arms off (measured, looked at). For four
subjects at FOV 40 and 16:9 use **`D` about 105**; for three, about 80; for five, about 130. Then
check 12.3 and, if `x0 < 3%` or `x1 > 97%`, increase `D` by 15% and recompute every `P_i` -- do not
reduce `dTheta`, that is how you get the blob back.

**The table above is for that ONE camera only** -- to check your arithmetic, not to copy
`POS_X`/`POS_Y` into a scene with a different camera. A different `C`, `phi` or `D` needs its own
`P_i`, computed fresh from the formula (see the troubleshooting note in 12.3).

### 12.2 The group's camera

- **Distance.** To frame N subjects at vertical FOV `F` and aspect `A`: the group spans
  `(N-1) * dTheta + w` degrees horizontally; the horizontal FOV is
  `FOV_h = 2 * atan(tan(F/2) * A)` (F 40 and 16:9 give 65.8). Keep the group within 55-70% of
  `FOV_h`.
- **Low angle without losing the ground.** `POS_Z = 0` is a literal low angle but leaves the ground
  at ~15% of the frame and the shadow unreadable. Round G's `POS_Z 10` aimed at z=20 crushed the
  shadow against the bottom edge (feet at 98.0%). Round H's `POS_Z 18-22` fixed the shadow and threw
  the low angle away (at `POS_Z` 20 the horizon crosses the subjects at chest height: that is eye
  level, not a low angle). Use **`POS_Z` 10-14 with `aim_z` 6-10** and
  `ROT_X = degrees(atan2(POS_Z - aim_z, D))` (about +2 to +3): the horizon then crosses the subjects
  between their knees and their hips, the feet land at 62-68% of the frame height, and the sun's
  cast shadow reads under every subject. See 13.6 for the full table and for what to tell the user.

### 12.3 Check BEFORE you render (mi3_screen_layout, measured thresholds)

Call `mi3_screen_layout` at the same aspect ratio you are about to render at, and require:

| criterion | threshold | line-up result | arc result (measured) |
|---|---|---|---|
| size spread `max(pct_h) / min(pct_h)` | **<= 1.25** | 2.10, FAILS | 1.064 |
| framing `abs(mean(center_pct_x) - 50)` | **<= 8** | 42.8 -> 7.2, just barely passes | 50.2 |
| no subject with `center_pct_x` < 8 or > 92 | -- | 12.05, at the limit | 21.6..78.8 |
| spacing between consecutive centers | **>= 12 points** | 24.9/18.6/11.2, uneven | 19.3, constant |
| `clipped_by_frame_edge` | 0 for every subject | H1 = 1, FAILS | 0 |
| feet on screen: `y1` of the lowest bbox | **58-72 %** of frame height (round H's 70-82 belongs to a camera at `POS_Z` 20; with the low camera of 13.6 the feet sit at 62-68 %) | 98.0, FAILS | 70.8-77.2 (round H camera) |
| headroom: `y0` of the highest bbox | **>= 8 %** of frame height (measured: with N=3 at `D`=80 and `POS_Z` 10 it comes out **-10.1 %**, with the heads cut off) | -- | -- |
| every bbox inside the frame: `x0` and `x1` | **x0 >= 3% and x1 <= 97%** of frame width | 2.6 / 93.4, just passes | 4.2 / 94.7 |

**`clipped_by_frame_edge` is not usable as a gate.** Measured: four subjects reported `0` while two
of them had `x0 = -7.2%` and `x1 = 105.0%` and visibly lost an arm off the edge. Use the `x0 >= 3%`
/ `x1 <= 97%` row above instead -- it is computed from the same bbox and it actually fires.

**Distance for N = 3 at a low camera:** `D` about **95**, not 80. Measured with the round I camera
(`POS_Z` 10, `aim_z` 6): three subjects at `D` = 80 put the head bbox at **-10.1%** of the frame and
cut the tops off. Four subjects at `D` = 105 land at +12.2% and five at `D` = 130 at +22.0%.

If anything fails, fix the NUMBERS and call `mi3_screen_layout` again: it costs nothing. Do not
spend a preview on a framing you already know is wrong, and do not call `mi3_render_quality`
while any row in this table still fails -- a quality render is minutes, not free, and confirming
a shot you already know is broken wastes it.

**Troubleshooting: if the size spread still fails after you have moved the camera and it does
not improve (or gets stuck 40%+ over the threshold), the camera is not the problem.** Re-read
12.1: each subject's position has to be computed from the camera's ACTUAL final `C`/`phi`. Go
back to the subjects' `POS_X`/`POS_Y` and recompute `P_i` for the camera you ended up with --
moving the camera around a set of subjects that were never positioned relative to it will
improve the number a little and then plateau, never reach the threshold, because the subjects
are simply not on an arc centered on that camera.

### 12.4 N figures, N silhouettes — ASSIGNED BY NUMBER (round H, tightened in round I)

Spacing stops the boxes touching. It does not stop N identical A-poses at identical height from
reading as a class photo. **This is an assignment, not a menu: subject i gets pose i.** "A heroic
group pose" is what the GROUP reads as, not one pose applied N times.

| subject (left to right on screen) | pose | `right_arm ROT_Y` | root `POS_Z` | what it does to the silhouette |
|---|---|---|---|---|
| 1 | `hero` (8.6) | **-22** | 0 | baseline: wide, symmetric, chest out |
| 2 | `fist up` (8.7) | **-115** | 0 | tallest: the raised arm adds height above the head line |
| 3 | `wide guard` (8.8) | **-46** | -1.5 | widest and slightly lower, arms bent away from the body |
| 4 | `kneel` (8.10) | **-30** | -5.48 | lowest: head 5.5 units down, one knee on the ground, torso upright |
| 5 | `idle` (8.1) | **-6** | 0 | relaxed, arms close to the body |

**Round L replaced pose 4 (`crouch` -> `kneel`):** a folded torso loses about half the key at
elevation 35 (2.266 spread vs 1.120 upright). `kneel` is just as low, keeps the chest up, and with
the round-L key (elevation 15-20) lands at 1.335-1.442. `crouch` (8.9) is still right if the user
asks for it -- say it reads softer.

**The `right_arm ROT_Y` column is your proof, and you must use it.** After you apply the poses,
read that channel back on every subject with `mi3_object_get` and compare the values. They must
all be DIFFERENT and must match the column. **If two subjects share the same `right_arm ROT_Y`,
one of them never got its pose: fix it and read it back again before you answer.** Two independent
runs of the same prompt with the same model produced, one of them, four byte-identical poses --
reading the channel back is what makes that impossible to ship.

Six or more subjects: you have five distinct poses. Reuse them and mirror the repeats
(section 4's rule: keep `ROT_X`, negate `ROT_Y`/`ROT_Z`, swap left/right parts) so no two
neighbours share a silhouette.

**Root height for any bent-leg pose.** The hip pivot ends up at
`6*cos(thigh) + 6*cos(shin)` with `shin = |thigh - BEND_ANGLE_X|`, so the root needs
`POS_Z = that - 12` or the character sinks into the ground. Verified against `state.dump`:
crouch(40,80) -> hip 9.19 (root -2.81); crouch(55,100) -> 7.68 (root -4.32); kneel(0,90) -> 6.00
(root -6.00); standing -> 12.00. Getting this wrong is not subtle: at root 0 and at root -12 the
same kneel read as a person buried to the waist (looked at, both).

**A one-knee kneel does NOT make the character shorter.** With the down thigh vertical the hips
stay at 12, exactly where they are standing. If you want a lower head, use the crouch (both knees),
not a kneel.

**Never stagger height with root `POS_Z` on characters standing on the ground.** The sun's cast
shadow is right there at their feet and it makes a 2-unit float obvious.

---

## 13. Lighting that IS VISIBLE (MEASURED, round G)

Everything below was measured with engine 1 (next-gen raster, the default) and through the same
render path your own two eyes use (`mi3_camera_preview` and `mi3_render_quality`).

### 13.1 What does NOT work (measured -- do not try these)

- **Raising the intensity.** A point light left at the default `LIGHT_RANGE` (250) is not a key
  light: it behaves like a global ambient. Turning it up erases the sun's cast shadow, which was
  the only directional cue there was. That shadow's contrast (shadow patch / lit ground): 0.81
  with no light, 0.90 at strength 1.4, 0.94 at 4, 0.98 at 10, 1.00 at 25 (and at 25 the frame is
  blown out to 249/255).
- **Putting the light on the camera axis.** Side-to-side contrast on the subject: 1.08 with NO
  light at all, and 0.98 / 0.95 / 0.93 with the light on-axis at strength 1.4 / 4 / 10 -- i.e.
  WORSE than no light. It just brightens and flattens.
- **`CAM_LIGHT_MANAGEMENT` + `CAM_TONEMAPPER` + `CAM_EXPOSURE` + `CAM_GAMMA`.** The render core
  forces `CAM_LIGHT_MANAGEMENT = 0` on the camera before every preview and every quality render,
  so **neither of your two eyes ever applies it.** Measured: AgX at exposure 1.0 and at 0.6
  produced byte-identical output to leaving the camera untouched. You can still set these if the
  user explicitly asks for them (they are real for their own viewport and their final export),
  but **say plainly that you cannot verify them yourself**, and never offer them as the fix for
  "I can't see the light."
- **Reaching the sun with `mi3_set_values`.** A newly created project has no Background object in
  the timeline, so `BG_SUNLIGHT_STRENGTH` and `BG_AMBIENT_COLOR` are not addressable that way --
  that dead end is real and always was. Since round K3 the sun, the ambient, the fog and the rest
  of the world panel are reachable through **`mi3_background_set`** instead (13.5), and
  `ensure_track: true` gives them a timeline track so they can be keyframed.

### 13.2 What DOES work: the azimuth

A character is boxes: four vertical faces with normals at `yaw+0/90/180/270`. The camera sees at
most two of them. A face's contribution is proportional to
`cos(face_azimuth - light_azimuth)`.

Sweep measured with the light FIXED and the character rotating (subject's mean gain over the
non-sky pixels):

| subject `ROT_Z` | 0 | 10 | 20 | 30 | 40 | 50 | 60 | 90 | 135 | 180 | -45 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| gain | +89% | +85% | +74% | +53% | +16% | 0% | +22% | +37% | 0% | +85% | 0% |

In other words: the SAME light goes from +89% to 0% just by turning the character. **Pick the
light's azimuth against the subject's yaw, not only against the camera.** Rotating `Body` plus
both legs instead of the root gives EXACTLY the same result -- there is no shortcut, and none is
needed.

### 13.3 Key light recipe for a GROUP (MEASURED AGAIN, round I -- replaces round H's)

Everything below uses `C_mid` and side `s` = +1 or -1 from 12.1 point 5. **Every angle below uses
the SAME `s`.**

1. **All subjects share ONE yaw: `ROT_Z = C_mid + 30 * s`** (12.1 point 5) -- the single change
   that closes the coverage hole; everything else here is worth about a tenth of it.
2. **Key azimuth `C_mid + 45 * s`** (band 40-50). At 32 the modelling drops; at 60 the coverage
   breaks (gain ratio 1.75, measured).
3. **Key at `1.6 * D` horizontally from the group's centre** (`D` = camera-to-group distance).
   Do NOT move it further: gain ratio 1.135 at `1.6 D`, 1.198 at `2.5 D`, 1.335 at `4.0 D` -- a
   distant key stops being 45 degrees off for the subjects at the ends of the arc.
4. **Key elevation 15** (band 12-20). Round I said 35, measured with four UPRIGHT subjects and the
   sun at default; round L re-measured on a delivered turn, engine 1, low pose in the group:
   spread **1.335 at 15**, 1.442 at 20, **1.552 at 25** (already failing), 1.699 at 30, **2.605 at
   35**. Costs upright nothing (1.120 at 35, 1.070 at 20) and makes the sun's ground shadow HARDER,
   not softer (contrast 0.652 at 15 vs 0.716 at 30). `POS_Z = 1.6 * D * tan(15) = 0.43 * D`.
5. **`LIGHT_RANGE = 2.0 * d_max`** (`d_max` = key to the FARTHEST subject). At `1.2 x` the far
   subject falls off a cliff (1.98); `2.0`, `3.0` and `6.0 x` render byte-identical. 2.0 is the
   plateau -- above it you buy nothing.
6. **`LIGHT_STRENGTH` comes from the GAIN you want, not from a distance formula** (round K3
   re-measured this; `d_max` only sets the scale). Sweep on the reference scene, `delta` 180:

   | `LIGHT_STRENGTH` | mean gain | spread | the sun's shadow contrast |
   |---|---|---|---|
   | `d_max / 300` | 63 % | 1.064 | **0.852** |
   | **`d_max / 200`** | **86 %** | **1.062** | **0.915** |
   | `d_max / 150` | 103 % | 1.069 | 0.964 -- shadow already gone |
   | `d_max / 100` | 124 % | 1.311 | 1.033 -- shadow inverted |
   | `d_max / 70` | 138 % | 1.567 | 1.079 |

   **`d_max / 200` is the ceiling if the sun's ground shadow has to survive.** Round I's "lead
   with `d_max / 150`" buys 17 more points of gain and costs the shadow. **Lowering the sun does
   NOT buy it back** (round K3 re-measured what this sheet used to claim): key at `/200`, sun at
   elevation 40 / 30 / 22 / 15, shadow contrast 0.836 / 0.860 / 0.896 / 0.943 -- a longer shadow
   is a WEAKER one. The only lever that works is a softer key. Gain % is about `99 + 51 * ln(u)`
   with `u = LIGHT_STRENGTH * 150 / d_max` (within 4 % over the sweep).

   Round L: measured with the DEFAULT ambient. With the group ambient of 13.5 (`23282F` +
   `sun_strength 1.8`), `d_max/200` gives subjects 76/124/92/102, contrast **0.656**, LESS clipping
   than default (0/18/2/4 % of matte at 250+ vs 0/22/15/7 %). Do not go `/150` to "compensate":
   +10 luminance costs 0.04 contrast, 1.311 -> 1.414 spread.
7. **Fill -- in a group shot it is not optional.** Azimuth `C_mid - 55 * s`, `1.4 * D` horizontally,
   elevation 18, `LIGHT_RANGE = 2.0 x its own d_max`, `LIGHT_STRENGTH = 25% of the key's`. With this
   recipe the fill costs nothing in modelling (worst lateral read 2.03 with and without it).
8. **Check before you render, all arithmetic and no pixels:** the N subjects' `ROT_Z` are the SAME
   number; the key's azimuth is `C_mid + 45*s` with the same `s` as that yaw; the fill exists and
   is 20-30% of the key; **and the sun is at `sun_azimuth = camera ROT_Z`** (13.5) -- without that
   last one the recipe is only valid near camera `ROT_Z` 180. Then take the two previews of 13.4,
   name the lit side on each subject, **and look for the sun's shadow UNDER EACH SUBJECT, one by
   one, not "in the frame"**. Round I passed with shadow in the frame and two of the four subjects
   standing on nothing: an aggregate number cannot catch that, only counting per subject can.
9. **Cohesion: do not let the anti-overlap arc keep opening.** The spacing factor `k` of 12.1
   decides whether four people read as a GROUP or as four people in a field. Measured, same shot,
   same light:

   | `k` | group width | neighbour overlap (points of frame) | key spread |
   |---|---|---|---|
   | 2.05 (round I) | 93 % of frame | 0.8 / 2.5 / **-1.5 (a gap)** | 1.069 |
   | **1.70** | **79 %** | **4.2 / 6.5 / 1.9** | **1.135** |
   | 1.45 | 69 % | 6.6 / 9.4 / 4.2 | **2.066** |
   | 1.25 | 61 % | 8.4 / 11.7 / 6.0 | -- |

   **`k` 1.70 is the sweet spot and 1.6 is the floor.** Closer than that and the near subjects
   shadow the far ones FROM THE KEY (point lights cast their own shadows, 13.5): at `k` 1.45 the
   gain splits 44/42 against 85/87. Raising the key does not rescue it -- measured at key elevation
   35 / 50 / 65 the spread goes 2.066 / 2.278 / 1.694 while the sun's shadow contrast decays
   0.902 / 0.931 / 0.957. If you want them tighter than `k` 1.6, stagger them in DEPTH instead of
   squeezing them sideways, and expect the size spread to grow (1.49 measured on a four-person
   wedge, against 1.14 at `k` 1.70).

**Worked example (round I reference: N = 4, D = 105, camera `ROT_Z` 180 at (0, 105, 12), `aim_z` 8
-> `ROT_X` +2.18, FOV 40, 16:9; `C_mid` = 0, `s` = +1, `d_max` = 223.6):**

| object | POS_X | POS_Y | POS_Z | ROT_Z | LIGHT_RANGE | LIGHT_STRENGTH |
|---|---|---|---|---|---|---|
| subject 1 (hero) | 43.89 | 3.86 | 0 | 30 | -- | -- |
| subject 2 (fist up) | 13.57 | 6.18 | 0 | 30 | -- | -- |
| subject 3 (wide guard) | -13.57 | 6.18 | -1.5 | 30 | -- | -- |
| subject 4 (crouch) | -43.89 | 3.86 | -2.81 | 30 | -- | -- |
| key | 118.79 | 118.79 | 117.63 | -- | 447.2 | 0.93 |
| fill | -120.42 | 84.32 | 47.76 | -- | 371.4 | 0.23 |

Distances from the key to the four subjects: 170.7 / 184.6 / 201.3 / 223.6. Measured on that exact
scene: per-subject gain +74 / +75 / +78 / +74% (**ratio 1.059**), lateral read 2.03 / 2.88 / 6.13 /
3.08, the sun's cast shadow readable under all four. Validated unchanged at `phi` 145 (1.061), with
three subjects (1.043) and with the camera at `POS_Z` 20 (1.059). This table uses the `d_max / 240`
floor strength (0.93) for a reproducible reference number; **default to `d_max / 200`** (point 6),
which on this exact scene is `LIGHT_STRENGTH` 1.12. Do NOT default to `d_max / 150`: on round K3's
star-gate scene a live turn used `/150` and the sun's shadow, geometrically under all four
subjects, was invisible under all four. **Look at the render, do not trust one contrast number:**
the same pair of images scores 0.967 or 0.870 depending only on how the ground mask is drawn, and
the eye says the shadow is gone either way.

**Two things this recipe does NOT cover (measured on that same live turn, round K3):**

- **A folded pose costs about half the key AT ELEVATION 35 -- the fix is to drop the key, not the
  pose.** K3 measured +43 % vs +80 % upright and concluded nothing helped, having only tried
  elevations 35/50/60. Round L swept downward on the same scene: folded goes +32/+45/+47/+54 % at
  key elevation 35/25/20/15 while upright stays flat (+72/+74/+76/+77 %), spread 2.266 -> 1.430.
  Use 8.10's `kneel` (12.4) and key elevation 15: measured 1.335 in engine 1.
- **A new project ships with a point light already in it** (`Point light`, strength 1,
  `LIGHT_RANGE` 250, near the origin). 13.1 says a 250-range point light behaves as a global
  ambient, and that is exactly what it does: measured, leaving it on lifted the whole frame by
  **21.4 %** and flattened it. Before you design any lighting, run `mi3_scene_summary` and either
  zero it or say out loud that you are keeping it -- do not just leave it there.

**Five subjects stretch it:** N = 5 at D = 130 measured a gain ratio of **1.292** and one subject
with no lateral read. It passes, but say so instead of promising three-or-four evenness.

### 13.4 How to CHECK the key actually reads (do not just assert it: look)

Take two cheap previews, one with `LIGHT_STRENGTH 0` and one at the value you chose, same
framing. Look at both and say which of the subject's two visible planes changed. If you cannot
tell the two images apart, the key is not doing anything -- **fix the azimuth first (13.2/13.3),
not the intensity: raising it only flattens the shot further.**

Reference for "how much is enough": in round G's reference shot, the key at strength 4 raised
the subject's mean luminance 61% over the same shot without the key, with a clear terminator and
no clipping. Above ~85% gain the highlights start to clip.

### 13.5 THE SUN, and how to aim it (RE-MEASURED PARAMETRICALLY, round K3)

Round H measured this with the sun bolted down (no tool to move it then). `mi3_background_set`
changes that -- the old table is now **one row** of a mechanism, all measured below.

**The two channels.** `sun_azimuth` is a compass in the SAME convention as camera `ROT_Z`
(0 = +Y, 90 = +X, 180 = -Y, 270 = -X) and says where the sun IS. `sun_elevation` is degrees above
the horizon. The default project sun is **azimuth 180, elevation 60** -- it sits on the -Y side.
(`sun_softness` is a third channel and is NOT a compass: it is the angular diameter of the sun
disc, 0.526 by default, i.e. how soft the shadow edge is. `sun_scale` is only the sprite in the
sky.)

**`sun_softness` RE-GOLDEN (round O3, PO-approved).** The channel used to be dropped on the way to
Cinema -- 0.526 and 8 rendered byte-identical -- so every pre-fix frame used one hardcoded softness.
It reaches Cinema now; penumbra grows LINEARLY with it (bar 46 units up: 0.526 deg = 0.6 units of
gradient, 1 = 1.1, 2 = 2.2, 4 = 4.4, 8 = 8.9). Old scenes LOOK DIFFERENT: the fix working. The old
look is exactly reproducible with **`sun_softness` 4.0** -- BYTE-IDENTICAL to the pre-fix render
(same sha256, whole sweep). No compatibility flag, ever.

**Cast shadow: direction and length.**

- direction = `sun_azimuth + 180`
- length = `height * cot(elevation)`

That is where round H's "0.577 x height" came from: `cot(60) = 0.577`. It is not an engine
constant. Measured in a top-down orthographic view against `32 * cot(elevation)`:

| sun elevation | 32-unit character throws | measured | error |
|---|---|---|---|
| 15 | 119.4 | 118.5 | -0.8 % |
| 30 | 55.4 | 56.2 | +1.4 % |
| 45 | 32.0 | 32.6 | +1.8 % |
| 60 | **18.5** | 18.4 | -0.3 % |
| 75 | 8.6 | 9.0 | +4.6 % |

The tool hands you `shadow_len_32` and `shadow_azimuth` already computed. Quote those.

**What the camera sees: `delta = camera_azimuth - sun_azimuth`** (`camera_azimuth = camera
ROT_Z + 180`, i.e. the azimuth of the camera as seen from the group -- the same `C_mid` as 13.3).
A face at `face_azimuth` receives `cos(face_azimuth - sun_azimuth)`; what you actually get in the
frame depends only on the difference. Measured on the four-subject reference scene, elevation 60,
no key light, `POS_Z` 12 / `aim_z` 8:

| `delta` | subject luminance | sun shadow in frame | subjects with their own shadow |
|---|---|---|---|
| 0 | **91.5** (max) | 3.1 % (min) | 0 / 4 |
| 45 | 79.6 | 3.4 % | 0 / 4 |
| 90 | 71.5 | 4.6 % | 2 / 4 |
| 135 | 68.2 | 6.0 % | 3 / 4 |
| **180** | 66.2 (min) | **6.4 % (max)** | **4 / 4** |
| 225 | 66.3 | 4.8 % | 2 / 4 |
| 270 | 79.8 | 4.5 % | 2 / 4 |
| 315 | 90.2 | 3.8 % | 0 / 4 |

The table is not symmetric because the subjects' shared yaw is `C_mid + 30 * s`: the half the
chests turn toward gets more light for the same shadow (compare 270 with 90).

**This replaces round H's table without contradicting it:** the same `delta`, produced either by
turning the rig (round H's method) or turning the sun, agree to **0.2 % luminance / 0.05 points
shadow area** across all eight deltas, and land within **2.3 %** of round H's own numbers
(93.2 / 81.5 / 70.4 / 66.9 / 65.7).

**THE CONSEQUENCE: camera azimuth is no longer a lighting decision.** Earlier rounds had to trade
the shot away because moving the camera moved `delta`. Now PIN the sun to the camera and shoot
where you like:

> **`sun_azimuth = camera ROT_Z`** puts `delta` at 180: the sun becomes a back light that lays the
> whole cast shadow into the frame, and your key does the modelling. Measured with the 13.3 group
> recipe at camera `ROT_Z` 135 / 180 / 225: spread **1.077 / 1.069 / 1.079**, shadow readable under
> **4 / 4** subjects in all three. Leave the sun at its default instead and the same recipe at
> `ROT_Z` 225 lands on `delta` 225 and loses the shadow under two of the four -- which is exactly
> the failure round I was rejected for.
>
> Want the sun ON the visible faces instead? `sun_azimuth = camera ROT_Z + 90` (`delta` 270) buys
> +21 % subject luminance, and costs you the key's modelling: at `delta` 270 the key's measured gain
> falls from 103 % to 54 % and the spread breaks to 1.55, because the sun is already lighting the
> faces the key was there to shape. Pick one on purpose and say which.

**Round L: `delta` alone is not enough -- sun ELEVATION decides whether the shadow exists at all**
(measured on a real delivered group turn, engine 1). Same camera (`ROT_Z` 135), sun moved:

| `delta` | sun elevation | shadow under each of the 4 (% of ground patch) | subjects with shadow |
|---|---|---|---|
| 135 (default sun, cam 135) | 60 | 0.0 / 0.0 / 0.0 / 0.0 | **0 / 4** |
| 180 | 60 | 2.3 / 2.6 / 2.1 / 2.4 | 4 / 4 (barely: bar is 2 %) |
| 160 / 180 / 200 | **40** | 8.7-11.0 / **10.2-11.6** / 8.3-11.2 | **4 / 4** |

For a group shot: **`sun_azimuth = camera ROT_Z` (delta 180, band 160-200) AND `sun_elevation 40`
(band 35-45)** -- 4-5x the default sun's shadow at the same delta.

**`sun_strength` is NOT a brightness knob.** Measured at `delta` 180, elevation 60, taking the sun
from 0.5 to 2.0 (four times the light):

| `sun_strength` \\ `ambient_color` | `23282F` (dark) | `66708C` (default) | `A0AAC0` (bright) |
|---|---|---|---|
| 0.5 | 0.662 / 44.8 | 0.813 / 65.7 | 0.882 / 77.9 |
| 1.0 | 0.543 / 45.4 | 0.691 / 66.2 | 0.768 / 78.4 |
| 2.0 | 0.515 / 46.1 | 0.660 / 66.8 | 0.735 / 78.9 |

(each cell = shadow contrast / subject luminance; contrast 1.00 = no shadow at all, lower = harder)

Read the rows and the columns: quadrupling the sun moved the subjects **+3 %** and the shadow
contrast 0.662 -> 0.515. Changing the ambient moved the subjects **+74 %**. At backlight the sun
does not light your subject -- **the ambient does**, and that is why a default scene reads flat.
**A flat-looking shot is fixed by a DARKER `ambient_color` plus a key, never by more sun.**

**The group "designed light" ambient (round L):** `ambient_color 23282F` + `sun_strength 1.8`, rest
per 13.3. Vs default `66708C`/1.0, same shot: key spread 1.420 -> **1.311**, shadow contrast
0.803 -> **0.656**, clipped pixels 0/22/15/7 % -> 0/18/2/4 %, luminance 90/158/107/116 ->
76/124/92/102. "Numbers pass" vs "reads as designed light": default ambient, same key/shadow,
still looks like default Minecraft. Say you darkened the ambient -- a look decision, not a fix.

**Still true:** `render_shadows` is ON by default; point and spot lights cast their own shadows in
their own direction (usually away from the camera, i.e. invisible) -- that is why a hot key makes
the ground look flat.

**Two engine gotchas if you measure the sun yourself:** (1) turning `render_shadows` on lowers the
level of the WHOLE ground, more so the lower the sun (on/off ratio over grass: 1.096 at elevation
75, 1.055 at 60, 0.987 at 45, 0.888 at 30, **0.736 at 15**), so always measure a shadow RELATIVE to
the lit ground of its own rows; (2) in ortho the engine does not clip behind the camera plane
(section 9), so a top-down shot renders the sun disc and the clouds into frame -- `sun_scale: 0`,
`moon.scale: 0` and `clouds.show: false` remove them without touching the LIGHT.

### 13.6 Low angle AND cast shadow: now you have a third lever (RE-MEASURED, round K3)

**Where the horizon lands is your camera's height and nothing else, and the sun does not enter
into it:** the horizon line crosses every subject at `POS_Z / 32` of the way up their body.
Checked against the pixel at eleven camera heights; prediction and measurement agree within 0.04.

| `POS_Z` | horizon crosses the body at | reads as |
|---|---|---|
| 2 | 6% (ankles) | extreme low angle |
| 6 | 19% (shins) | low angle |
| **10** | **31% (knees)** | **low angle** |
| **12** | **38% (thighs)** | **low angle** |
| **14** | **44% (hips)** | **low angle** |
| 20 | 62% (chest) | **eye level** -- round H's recipe, and it is not a low angle |
| 26 | 81% (shoulders) | slightly high |

**The window is still `POS_Z` 10-14 with `aim_z` 6-10** (`ROT_X = degrees(atan2(POS_Z - aim_z, D))`,
about +2 to +3). Default to `POS_Z` 12, `aim_z` 8.

Round I could not get the shadow to read from down there without raising the camera. It never
needed the camera: **it needed a lower sun.** Same shot, same key, `delta` 180, shadow under each
subject as a fraction of their own patch of ground:

| sun elevation | shadow length of a 32-unit body | shadow under each of the 4 |
|---|---|---|
| 60 (default) | 18.5 | 3.9 - 6.3 % |
| **40 -- group closing-protocol value (round L)** | **38.1** | **14.6 - 16.2 %** |
| 25 | 68.6 | 57.8 - 63.1 % |

Elevation 35-45 is the working band: two to four times the shadow of the default sun, still
daylight, and the frame does not turn into a sunset. Below about 9 degrees the engine's night
blend starts (that is what makes a real sunset -- see the `twilight` field).

**Never quietly deliver an eye-level shot when the user said "low angle".** If they ask for
something the geometry cannot give -- a camera pitched UP (`ROT_X` negative) throws the ground and
its shadows out of frame, measured 3.5 % -> 1.6 % of the frame -- do it, then say what it cost.

## 14. Studio backdrop (MEASURED, round J)

The sky and the ground are NOT objects. They are project settings -- the "Background / World"
panel -- and you reach them with `mi3_background_set`, never with `mi3_set_values`. There is no
`ref` for them; if you go looking for one you will not find it.

### 14.1 The white studio, exactly

```
mi3_background_set(sky_color: "#FFFFFF", ground_block: "block/white_concrete", clouds_show: false)
```

Measured on an empty scene, next-gen engine, 320x180, sun untouched:

| | pixel value |
|---|---|
| sky | **#FFFFFF** (exactly, not "nearly") |
| floor | **#E7EDEF** (about 24/255 below the sky on every channel) |

That 24-point gap is the whole point: the floor reads as a separate surface, with no light touched.
The default `fog_show`/`fog_sky` melt the far floor into the sky, so there is no seam at the back:
leave the fog alone.

Clouds MUST be off. With clouds on, the top third of the frame comes back as noise between
#DEE1E5 and #F0F3F7 instead of flat white.

### 14.2 Floors that DO NOT work (measured -- do not try these)

| `ground_block` | floor renders as | verdict |
|---|---|---|
| `block/white_wool` | #FFFFFF | **invisible** -- identical to the sky |
| `block/quartz_block_top` | #FFFFFF | **invisible** -- identical to the sky |
| `ground_show: false` | #FFFFFF everywhere | an infinite white void: no horizon, no contact shadow |

For a *grey* studio the measured options are `block/smooth_stone` (floor about #B0B0B0-#C2C2C2) and
`block/light_gray_concrete` (about #8C8C81) -- proper greys, not "very light grey". Say which one
you picked and why.

Block names come from the Minecraft texture catalogue (854 entries here) and are matched by NAME,
with or without the `block/` prefix. If you get the name wrong the tool answers with up to six
real names that contain what you typed -- read them and retry once, do not guess twice.

### 14.3 The rest of the panel (round K3: the sun opened)

`mi3_background_set` now covers the whole Background / World panel -- sun, ambient, night, fog,
clouds, moon, `biome`, `wind`; its own parameter list is the catalogue, no need to repeat it here.
The geometry and the recipes that go with the sun live in 13.5 and 13.6 -- read them before you
move it, because they are written for the DEFAULT sun and tell you how to recompute for any other.

`ensure_track: true` creates the timeline's BACKGROUND track; the mechanics are in the tool's own
description. Measured: the render before and after creating it is **byte-identical**, and a sunset
of elevation 60 at frame 0 -> elevation 4 at frame 48 drops mean frame luminance 49.5 %.

Still out of scope: a custom background image, the ground's material/normal maps, and the
per-biome colour overrides (`grass`, `foliage`, `water`, the seven leaf colours): `biome` sets them
all together and they are not exposed one by one.

### 14.4 Undo

One call = **one** Ctrl+Z. Batch related fields so one undo restores the whole edit.

## 15. Armor trim EFFECTS (20 -- catalog, not vibes)

`mi3_object_add kind:"armor"` and `mi3_object_edit op:"trim"` take `pattern` + `material`
(the baked trim) and `effect` (one of these 20, or `none`). "In the dark": `lit` never goes
fully dark; `pulses` goes black for part of its cycle. All 20 feed bloom.

| effect | look | in the dark |
|---|---|---|
| glowing | flat fullbright, no motion | lit |
| periodic | slow whole-piece pulse | pulses |
| shimmer | faintest of the 20, soft sheen | pulses |
| aurora | vertical green-to-violet curtains | pulses |
| bodysweep | bright bar sweeping up the body, 5.5 s | lit |
| breath | calm 4 s in-and-out glow, pieces in sync | lit |
| charge | per-texel buckets filling, then a flash | pulses |
| chrome | near-constant hard metal sheen | lit |
| kaleido | rotating kaleidoscope wedges | lit |
| lava_lamp | slow warm blobs rising | lit |
| molten | cracked crust with hot seams | pulses |
| nebula | slow interference clouds | lit |
| nocturne | dead by day, burns at night | pulses |
| oilslick | thin-film iridescence, always alive | lit |
| opal | pastel pearl sliding diagonally | lit |
| plasma | magma-cream churn | lit |
| rainbow_flow | fast full-spectrum flow | lit |
| silk | pearly S-waves folding like silk | lit |
| spark | 8 time slices of scattered glints | lit |
| tide | swell with a foam line on each crest | pulses |
