// ASSISTANT/test/fake-editor.mjs
//
// A minimal stand-in for the GML editor side of the protocol (spec section 3
// and 4), so the brain (this repo's actual deliverable) can be tested end to
// end without depending on the parallel GmProject/CppProject work. It plays
// BOTH roles the real editor plays:
//
//   - producer of chat_in.jsonl (hello/user/stop/model), consumer of
//     chat_out.jsonl -- with the exact claim-by-rename handshake from
//     section 3.3, so timing bugs on either side show up here too.
//   - consumer of ops_req.json / producer of ops_res.json, running a small
//     in-memory scene ("Cubo" cube, "Camara" camera, "Luz" point light) that
//     genuinely gets mutated by assistant.values_set / edit.undo / etc., so
//     tests can assert real before/after state, not just "did it not throw".
//
// This is intentionally NOT a faithful reimplementation of every GML quirk
// -- it exists to validate the brain's side of the contract.

import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { makeSolidPng } from './png.mjs';

function stripBom(s) {
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s;
}

function cloneObj(o) {
  return JSON.parse(JSON.stringify(o));
}

// RONDA K1: espejo (deliberadamente simplificado, ver cabecera del fichero)
// de la tabla de ~60 campos de mi3_as_pset_fields() (mi3_assistant_project.gml).
// No modela cada default real del engine -- existe para ejercitar el
// CONTRATO de assistant.project_get/assistant.project_set (nombres validos,
// "applied"/"skipped", una entrada de historial por lote). Los valores reales
// se verifican contra el motor de verdad (gates G1.1/G1.2 del spec).
const PSET_FIELDS = {
  engine: 1,
  samples: 64,
  ssaa: 1,
  render_distance: 30000,
  shadows: 1,
  shadows_sun_buffer: 2048,
  shadows_spot_buffer: 512,
  shadows_point_buffer: 256,
  shadows_transparent: 1,
  ssao: 1,
  ssao_radius: 12,
  ssao_power: 1,
  ssao_color: '000000',
  indirect: 1,
  indirect_precision: 0.3,
  indirect_blur: 1,
  indirect_strength: 1,
  reflections: 1,
  reflections_precision: 0.3,
  reflections_thickness: 1,
  reflections_fade: 1,
  glow: 1,
  glow_radius: 1,
  glow_intensity: 1,
  glow_falloff: 0,
  glow_falloff_radius: 2,
  glow_falloff_intensity: 1,
  aa: 1,
  aa_power: 1,
  tonemapper: 0,
  exposure: 1,
  gamma: 2.2,
  ambient_strength: 1,
  face_shade: 0,
  bend_style: 0,
  subsurface_samples: 7,
  subsurface_highlight: 0.5,
  subsurface_highlight_strength: 1,
  block_subsurface: 8,
  opaque_leaves: 0,
  liquid_animation: 1,
  water_reflections: 0,
  block_emissive: 1,
  material_maps: 0,
  glint_speed: 1,
  glint_strength: 1,
  texture_filtering: 0,
  texture_filtering_level: 1,
  transparent_block_texture_filtering: 0,
  alpha_mode: 0,
  overscan: 0,
  tiled: 0,
  cinema_bounces: 4,
  cinema_noise: 0,
  cinema_denoise: 1,
  cinema_clip: 0,
  cinema_env_light: 0,
  cinema_backdrop: 0,
  cinema_backdrop_color: 16777215,
  cinema_backdrop_color2: 16777215,
  cinema_backdrop_angle: 0,
  tempo: 20,
  video_width: 1920,
  video_height: 1080,
  video_keep_aspect: 1,
  video_template: 0,
};

function initialScene() {
  return [
    { saveId: 1, name: 'Cubo', type: 'cube', parent: '', pos: [0, 0, 0], rot: [0, 0, 0], sca: [1, 1, 1], kf: 0, keyframes: [] },
    {
      saveId: 2,
      name: 'Camara',
      type: 'camera',
      parent: '',
      pos: [0, -10, 5],
      rot: [70, 0, 0],
      kf: 0,
      keyframes: [],
      fov: 45,
      ortho: false,
      active: true,
      hide: false,
      lightManagement: 1,
    },
    {
      saveId: 3,
      name: 'Luz',
      type: 'point_light',
      parent: '',
      pos: [5, 5, 5],
      rot: [0, 0, 0],
      kf: 0,
      keyframes: [],
      strength: 1,
      range: 20,
    },
  ];
}

export class FakeEditor {
  constructor(dir, opts = {}) {
    this.dir = path.resolve(dir);
    this.previewDir = path.join(this.dir, 'preview');

    this.receivedOut = []; // every chat_out record ever claimed, in order
    this.opsExecuted = 0; // RONDA J: contador simple de ops _execOp -- para
    // afirmar "el fake no recibio ningun op" sin depender de inferirlo del
    // estado de la escena (J1.c).
    this.lastOps = []; // RESCATE K5: {op, args} de cada op recibido, en orden
    this.seqInCounter = 0;
    this.activeTurn = null;
    this.checkpointedTurns = new Set();
    this.autoCheckpoints = [];
    this.manualCheckpoints = [];
    this._cpCounter = 1;
    this.historyStack = [];
    this.historyPos = 0;
    this.scene = initialScene();
    this.selected = null;
    this.state = {
      marker: 0,
      timelineLength: 200,
      tempo: 20,
      renderEngine: 'legacy',
      renderSamples: 64,
      groundShow: 1,
      fogShow: 0,
      skyTime: 12,
      sunlightStrength: 1,
      projectSaved: opts.projectSaved ?? true,
      projectFile: opts.projectFile ?? 'assistest.miproject',
      busy: false,
      popup: false,
      // RONDA K4: raices designadas que dependen del EDITOR (assistant.paths).
      projectsFolder: opts.projectsFolder ?? 'C:\\FakeProjects',
      projectFolder: opts.projectFolder ?? 'C:\\FakeProjects\\assistest',
    };
    // RONDA K4: catalogo de sticker packs falso para tests sin el motor real.
    this.stickerPacks = opts.stickerPacks ?? { TestPack: ['sword.png'] };

    // RONDA J: fondo del proyecto (assistant.background_set). Defaults
    // medidos por el director contra el motor real (project_reset_background,
    // SPEC_ASSISTANT_RONDA_J.md S1.4/1.8): cielo 78A7FF, suelo
    // block/grass_block_top.
    // RONDA K3: el resto del panel (sol, ambiente, niebla, nubes, luna,
    // bioma, viento), con los defaults exactos de project_reset_background.
    this.background = {
      sky_color: '78A7FF',
      clouds_color: 'FFFFFF',
      clouds_show: 1,
      ground_show: 1,
      ground_block: 'block/grass_block_top',
      fog_show: 1,
      fog_sky: 1,
      sky_time: 30,
      sky_rotation: 0,
      sunlight_strength: 1,
      sunlight_color: 'FFF7E4',
      sunlight_angle: 0.526,
      sky_sun_scale: 1,
      twilight: 1,
      ambient_color: '66708C',
      night_color: '0E0E18',
      fog_distance: 10000,
      fog_size: 2000,
      fog_height: 1250,
      clouds_speed: 1,
      clouds_height: 1024,
      clouds_offset: 0,
      moon_phase: 0,
      moon_angle: 0,
      moon_scale: 1,
      biome: 'plains',
      wind: 1,
      wind_speed: 0.1,
      wind_strength: 0.5,
      wind_direction: 45,
    };
    // Pista BACKGROUND del timeline: mientras no exista, los ajustes viven en
    // el proyecto; en cuanto existe, cada escritura es un keyframe.
    this.bgTrack = false;
    this.bgKeyframes = [];

    // RONDA K1: ajustes de proyecto/render (assistant.project_get/_set) y
    // presets (project.load_preset) -- ver PSET_FIELDS mas arriba.
    this.projectSettings = { ...PSET_FIELDS };
    this.presets = {
      performance: { ...PSET_FIELDS },
      cinematic: {
        samples: 128,
        tonemapper: 1,
        exposure: 1.1,
        gamma: 2.4,
        indirect_precision: 0.6,
        indirect_strength: 1.3,
        reflections_precision: 0.6,
        reflections_thickness: 1.5,
        glow_intensity: 1.2,
        aa_power: 2,
      },
    };

    // RONDA K1: marcadores de escena (assistant.markers).
    this.markers = [];

    this.beatSeen = -1;
    this.beatMisses = 0;
    this.conn = 'off';

    this._timers = [];
    this._opsReqParseFailures = 0;
  }

  // -------------------------------------------------------------------
  // lifecycle
  // -------------------------------------------------------------------

  start() {
    fs.mkdirSync(this.dir, { recursive: true });
    fs.mkdirSync(this.previewDir, { recursive: true });
    // Startup hygiene, mirroring mi3_assistant_startup (spec 4.2).
    for (const f of [
      'chat_out.flag',
      'chat_out.jsonl',
      'chat_out.take',
      'chat_in.flag',
      'chat_in.jsonl',
      'chat_in.take',
      'ops_req.json',
      'ops_res.json',
      'brain.json',
    ]) {
      try {
        fs.unlinkSync(path.join(this.dir, f));
      } catch {
        /* fine if absent */
      }
    }
    this._timers.push(setInterval(() => this._pollOut(), 20));
    this._timers.push(setInterval(() => this._pollOpsReq(), 15));
    this._timers.push(setInterval(() => this._pollHeartbeat(), 1000));
  }

  stop() {
    for (const t of this._timers) clearInterval(t);
    this._timers = [];
  }

  // -------------------------------------------------------------------
  // chat_in producer
  // -------------------------------------------------------------------

  _writeIn(rec) {
    this.seqInCounter++;
    const full = { seq: this.seqInCounter, t: Date.now(), ...rec };
    fs.appendFileSync(path.join(this.dir, 'chat_in.jsonl'), JSON.stringify(full) + '\n', 'utf8');
    fs.writeFileSync(path.join(this.dir, 'chat_in.flag'), '', 'utf8');
    return full;
  }

  sendHello() {
    return this._writeIn({ type: 'hello', editor_version: '3', project: this.state.projectFile });
  }

  sendUser(text) {
    const turnId = randomUUID();
    this.activeTurn = turnId;
    // RONDA E: cada turno manda el proyecto activo (no solo el hello de
    // conexion) para que abrir otro proyecto sin relanzar el cerebro no deje
    // la memoria de proyecto rancia. "" cuando el proyecto nunca se ha
    // guardado (mismo significado que projectFile por defecto).
    this._writeIn({ type: 'user', turn_id: turnId, text, project_file: this.state.projectFile });
    return turnId;
  }

  /** RONDA E: cambia el proyecto "abierto" en caliente (usado por el test de aislamiento). */
  setProjectFile(projectFile) {
    this.state.projectFile = projectFile;
    this.state.projectSaved = true;
  }

  sendModel(model) {
    return this._writeIn({ type: 'model', model });
  }

  /** Mirrors action_assistant_stop (A5): writes stop AND immediately drops any pending ops_req. */
  sendStop(turnId) {
    this._writeIn({ type: 'stop', turn_id: turnId });
    if (this.activeTurn === turnId) this.activeTurn = null;
    try {
      fs.unlinkSync(path.join(this.dir, 'ops_req.json'));
    } catch {
      /* nothing pending, fine */
    }
  }

  // -------------------------------------------------------------------
  // chat_out consumer (claim by rename, section 3.3)
  // -------------------------------------------------------------------

  _pollOut() {
    const flag = path.join(this.dir, 'chat_out.flag');
    if (!fs.existsSync(flag)) return;
    try {
      fs.unlinkSync(flag);
    } catch {
      return;
    }
    const jsonl = path.join(this.dir, 'chat_out.jsonl');
    const take = path.join(this.dir, 'chat_out.take');
    try {
      fs.renameSync(jsonl, take);
    } catch {
      fs.writeFileSync(flag, '', 'utf8'); // recreate; retry next tick
      return;
    }
    let raw;
    try {
      raw = stripBom(fs.readFileSync(take, 'utf8'));
    } finally {
      try {
        fs.unlinkSync(take);
      } catch {
        /* already gone */
      }
    }
    for (const rawLine of raw.split('\n')) {
      const line = rawLine.replace(/\r$/, '').trim();
      if (!line) continue;
      let rec;
      try {
        rec = JSON.parse(line);
      } catch {
        continue;
      }
      this.receivedOut.push(rec);
    }
  }

  // -------------------------------------------------------------------
  // ops_req consumer / ops_res producer (section 3.7 / 4.4)
  // -------------------------------------------------------------------

  _pollOpsReq() {
    const reqPath = path.join(this.dir, 'ops_req.json');
    if (!fs.existsSync(reqPath)) return;
    let req;
    try {
      req = JSON.parse(stripBom(fs.readFileSync(reqPath, 'utf8')));
    } catch {
      this._opsReqParseFailures++;
      return; // still mid-write; try again next tick
    }
    const res = this._processOpsReq(req);
    const resPath = path.join(this.dir, 'ops_res.json');
    const tmp = resPath + '.part';
    fs.writeFileSync(tmp, JSON.stringify(res), 'utf8');
    fs.renameSync(tmp, resPath);
    try {
      fs.unlinkSync(reqPath);
    } catch {
      /* already consumed */
    }
  }

  _processOpsReq(req) {
    const { seq, turn_id: turnId, ops } = req;

    if (turnId && this.activeTurn !== null && turnId !== this.activeTurn) {
      return { seq, turn_id: turnId, ok: 0, ms: 0, ops_total: 0, ops_ok: 0, error: 'turno cancelado', results: [] };
    }

    const t0 = Date.now();
    let checkpoint;
    if (turnId && !this.checkpointedTurns.has(turnId)) {
      this.checkpointedTurns.add(turnId);
      checkpoint = this._createAutoCheckpoint();
    }

    const results = [];
    let opsOk = 0;
    for (const op of ops ?? []) {
      const r = this._execOp(op);
      results.push(r);
      if (r.ok) opsOk++;
    }

    const ms = Date.now() - t0;
    return {
      seq,
      turn_id: turnId,
      ok: opsOk === (ops ?? []).length ? 1 : 0,
      ms,
      ops_total: (ops ?? []).length,
      ops_ok: opsOk,
      checkpoint,
      results,
    };
  }

  _createAutoCheckpoint() {
    if (!this.state.projectSaved) return { created: 0, error: 'nofolder' };
    const id = 'cp' + String(this._cpCounter++).padStart(4, '0');
    const name = 'assistant ' + new Date().toISOString();
    this.autoCheckpoints.push({ id, name, snapshot: cloneObj(this.scene) });
    if (this.autoCheckpoints.length > 5) this.autoCheckpoints.shift(); // pool of 5, evict oldest AUTO only
    return { created: 1, cp_id: id, name };
  }

  // -------------------------------------------------------------------
  // Scene model + op handlers (assistant.* + edit.undo)
  // -------------------------------------------------------------------

  findObject(ref) {
    if (!ref) return null;
    return (
      this.scene.find((o) => String(o.saveId) === String(ref)) ||
      this.scene.find((o) => o.name === ref) ||
      this.scene.find((o) => o.name.toLowerCase() === String(ref).toLowerCase()) ||
      null
    );
  }

  _execOp(op) {
    const { id, op: name, args = {} } = op;
    this.opsExecuted++;
    // RESCATE K5: bitacora cruda de lo que el ANFITRION mando de verdad (no de
    // lo que el resultado echa de vuelta). La usa el test de toEditorPath para
    // comprobar que ninguna ruta cruza al editor con "\".
    this.lastOps.push({ op: name, args });
    try {
      switch (name) {
        case 'assistant.scene_summary':
          return this._opSceneSummary(id, name, args);
        case 'assistant.object_get':
          return this._opObjectGet(id, name, args);
        case 'assistant.select':
          return this._opSelect(id, name, args);
        case 'assistant.marker':
          return this._opMarker(id, name, args);
        case 'assistant.values_set':
          return this._opValuesSet(id, name, args);
        case 'assistant.camera_preview':
          return this._opCameraPreview(id, name, args);
        case 'assistant.render_snapshot':
          return this._opRenderSnapshot(id, name, args);
        case 'assistant.checkpoint':
          return this._opCheckpoint(id, name, args);
        case 'assistant.ping':
          return this._opPing(id, name, args);
        case 'assistant.screen_layout':
          return this._opScreenLayout(id, name, args);
        case 'assistant.object_add':
          return this._opObjectAdd(id, name, args);
        case 'assistant.object_delete':
          return this._opObjectDelete(id, name, args);
        case 'object.duplicate':
          return this._opObjectDuplicate(id, name, args);
        case 'assistant.background_set':
        case 'assistant.env_set':
          return this._opBackgroundSet(id, name, args);
        case 'assistant.project_get':
          return this._opProjectGet(id, name, args);
        case 'assistant.project_set':
          return this._opProjectSet(id, name, args);
        case 'assistant.preset_list':
          return this._opPresetList(id, name, args);
        case 'project.load_preset':
          return this._opPresetLoad(id, name, args);
        case 'timeline.set_length':
          return this._opTimelineSetLength(id, name, args);
        case 'assistant.markers':
          return this._opMarkers(id, name, args);
        case 'edit.undo':
          return this._opUndo(id, name, args);
        case 'edit.redo':
          return this._opRedo(id, name, args);
        case 'assistant.keyframes':
          return this._opKeyframes(id, name, args);
        case 'assistant.object_edit':
          return this._opObjectEdit(id, name, args);
        case 'armor.set_trim':
          return this._opSetTrim(id, name, args);
        // ShinyTrims O7 (tool trims): los ops de ITEM son OTROS ops, no una
        // rama de armor.set_trim -- el fake los distingue igual que el motor,
        // o el test "y NO armor.set_trim" no probaria nada.
        case 'item.set_trim':
          return this._opSetItemTrim(id, name, args);
        case 'item.get_trim':
          return this._opGetItemTrim(id, name, args);
        case 'object.parent':
          return this._opParent(id, name, args);
        case 'transform.copy':
          return this._opTransformCopy(id, name, args);
        case 'transform.paste':
          return this._opTransformPaste(id, name, args);
        case 'object.mirror_pose':
          return this._opMirrorPose(id, name, args);
        case 'camera.focus_on':
          return this._opFocusOn(id, name, args);
        case 'camera.ortho_toggle':
          return this._opOrthoToggle(id, name, args);
        case 'assistant.paths':
          return this._opPaths(id, name, args);
        case 'assistant.library':
          return this._opLibrary(id, name, args);
        case 'assistant.sticker_add':
          return this._opStickerAdd(id, name, args);
        case 'asset.import':
          return this._opAssetImport(id, name, args);
        case 'res.import':
          return this._opResImport(id, name, args);
        case 'object.set_skin':
          return this._opSetSkin(id, name, args);
        case 'assistant.skin_apply':
          return this._opSkinApply(id, name, args);
        case 'world.import':
          return this._opWorldImport(id, name, args);
        // RONDA K5: gestion de fichero de proyecto (mi3_project_file). Estado
        // "sucio" = this.historyStack (assistant.ping ya lo lee asi, ver
        // _opPing mas abajo) -- abrir/nuevo/guardar/restaurar lo VACIAN,
        // igual que en el motor real (el estado en memoria vuelve a coincidir
        // con lo que hay en disco justo despues de cada uno de estos).
        case 'project.open':
          return this._opProjectOpen(id, name, args);
        case 'project.new':
          return this._opProjectNew(id, name, args);
        case 'project.save':
          return this._opProjectSave(id, name, args);
        case 'project.checkpoint_restore':
          return this._opProjectCheckpointRestore(id, name, args);
        default:
          return { id, op: name, ok: 0, error: 'op desconocido: ' + name };
      }
    } catch (e) {
      return { id, op: name, ok: 0, error: String((e && e.message) || e) };
    }
  }

  _opSceneSummary(id, name, args) {
    const maxObjects = args.max_objects ?? 200;
    const rows = this.scene.slice(0, maxObjects);
    const objectsText = rows
      .map((o) => `${o.saveId}|${o.name}|${o.type}|${o.parent || ''}|${o.pos.join(',')}|${o.rot.join(',')}|${o.kf}`)
      .join(';');
    const cameras = this.scene.filter((o) => o.type === 'camera');
    const camerasText = cameras.map((c) => `${c.saveId}|${c.name}|${c.fov}|${c.ortho ? 1 : 0}|${c.active ? 1 : 0}`).join(';');
    const lights = this.scene.filter((o) => o.type.includes('light'));
    const lightsText = lights.map((l) => `${l.saveId}|${l.name}|${l.type}|${l.strength}|${l.range}`).join(';');
    return {
      id,
      op: name,
      ok: 1,
      project: this.state.projectSaved ? this.state.projectFile : '(sin guardar)',
      marker: this.state.marker,
      timeline_length: this.state.timelineLength,
      tempo: this.state.tempo,
      render_engine: this.state.renderEngine,
      render_samples: this.state.renderSamples,
      count: this.scene.length,
      objects: objectsText,
      cameras: camerasText,
      lights: lightsText,
      // RONDA J: 9 campos, mismo orden y significado que el op real
      // (mi3_assistant_ops.gml S3.5): ground_show|fog_show|sky_time|
      // sunlight_strength|sky_color_hex|ground_block|clouds_show|fog_sky|via.
      // El fake nunca modela un objeto BACKGROUND en el timeline -- "via" es
      // siempre "app" (ver el header del fichero: no es una reimplementacion
      // fiel de cada quirk de GML, solo el contrato que ejercitan las tools).
      background: [
        this.background.ground_show,
        this.background.fog_show,
        this.state.skyTime,
        this.state.sunlightStrength,
        this.background.sky_color,
        this.background.ground_block,
        this.background.clouds_show,
        this.background.fog_sky,
        'app',
      ].join('|'),
      truncated: this.scene.length > maxObjects ? 1 : 0,
    };
  }

  _opObjectGet(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada' };
    return {
      id,
      op: name,
      ok: 1,
      target: o.name,
      type: o.type,
      parent: o.parent || '',
      children: this.scene
        .filter((c) => c.parent === o.name)
        .map((c) => c.name)
        .join(','),
      pos_x: o.pos[0],
      pos_y: o.pos[1],
      pos_z: o.pos[2],
      rot_x: o.rot[0],
      rot_y: o.rot[1],
      rot_z: o.rot[2],
      has_pos: 1,
      has_rot: 1,
      has_sca: o.type === 'camera' ? 0 : 1,
      hide: o.hide ? 1 : 0,
      keyframes: String(o.kf),
    };
  }

  _opSelect(id, name, args) {
    this.selected = args.ref ? this.findObject(args.ref) : null;
    return { id, op: name, ok: 1, selected: this.selected ? this.selected.name : '' };
  }

  _opMarker(id, name, args) {
    this.state.marker = Math.round(args.frame ?? 0);
    return { id, op: name, ok: 1, marker: this.state.marker };
  }

  _opValuesSet(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada' };
    const values = args.values || {};
    const before = cloneObj(o);
    let applied = 0;
    const skipped = [];

    for (const [k, v] of Object.entries(values)) {
      const posAxis = { POS_X: 0, POS_Y: 1, POS_Z: 2 }[k];
      const rotAxis = { ROT_X: 0, ROT_Y: 1, ROT_Z: 2 }[k];
      const scaAxis = { SCA_X: 0, SCA_Y: 1, SCA_Z: 2 }[k];
      if (posAxis !== undefined) {
        o.pos[posAxis] = Number(v);
        applied++;
      } else if (rotAxis !== undefined) {
        o.rot[rotAxis] = Number(v);
        applied++;
      } else if (scaAxis !== undefined) {
        if (o.type === 'camera') {
          skipped.push(k); // cameras have no scale channel
          continue;
        }
        o.sca[scaAxis] = Number(v);
        applied++;
      } else if (k === 'CAM_FOV' && o.type === 'camera') {
        o.fov = Number(v);
        applied++;
      } else {
        o[k] = v;
        applied++;
      }
    }

    if (applied === 0) return { id, op: name, ok: 0, error: 'ninguna propiedad aplicable' };

    const kfBefore = o.kf;
    o.kf = o.kf + 1;
    // RONDA K2: espejo simplificado de "keyframe.list ampliado" -- si el
    // marcador ya tiene un keyframe en o.keyframes se fusiona (mismo criterio
    // que tl_keyframe_add real: "encuentra hueco o edita el que hay"), si no
    // se crea uno nuevo con transicion "linear" por defecto.
    if (!o.keyframes) o.keyframes = [];
    let kfEntry = o.keyframes.find((k) => k.pos === this.state.marker);
    if (!kfEntry) {
      kfEntry = { pos: this.state.marker, transition: 'linear', values: {} };
      o.keyframes.push(kfEntry);
      o.keyframes.sort((a, b) => a.pos - b.pos);
    }
    Object.assign(kfEntry.values, values);
    this._pushHistory({ name: o.name, before });

    return {
      id,
      op: name,
      ok: 1,
      target: o.name,
      applied,
      kf_before: kfBefore,
      kf_after: o.kf,
      kf_created: 1,
      marker: this.state.marker,
      skipped: skipped.join(','),
    };
  }

  // RONDA J: assistant.background_set. Mismo espiritu que _opValuesSet:
  // rechaza los campos excluidos (sol / cielo horario / ambiente) con ok:0,
  // rechaza un ground_block que no venga con el prefijo "block/" (el fake NO
  // modela el catalogo real de 854 texturas -- eso se prueba contra el motor
  // real, gate J2.b1), aplica lo demas sobre this.background y devuelve
  // {applied, via:"app", undoable:1, ...this.background}. No modela
  // historial/undo (eso se prueba contra el editor REAL, gates J2/J2.b);
  // aqui solo se ejercita el CONTRATO del op para mi3_background_set.
  // RONDA K3: mismo contrato que mi3_as_op_background_set / assistant.env_set.
  // Clave -> campo canonico. sun_azimuth/sun_elevation son la cara amable de
  // sky_rotation/sky_time (azimut = rotation + 180, elevacion = 90 - time),
  // exactamente como en el motor (background_sky_update_sun.gml).
  static ENV_MAP = {
    sky_color: 'sky_color',
    ground_show: 'ground_show',
    ground_block: 'ground_block',
    clouds_show: 'clouds_show',
    clouds_color: 'clouds_color',
    clouds_speed: 'clouds_speed',
    clouds_height: 'clouds_height',
    clouds_offset: 'clouds_offset',
    fog_show: 'fog_show',
    fog_sky: 'fog_sky',
    fog_color: 'fog_color',
    fog_object_color: 'fog_object_color',
    fog_distance: 'fog_distance',
    fog_size: 'fog_size',
    fog_height: 'fog_height',
    sun_strength: 'sunlight_strength',
    sun_color: 'sunlight_color',
    sun_softness: 'sunlight_angle',
    sun_scale: 'sky_sun_scale',
    sun_time: 'sky_time',
    sun_rotation: 'sky_rotation',
    twilight: 'twilight',
    ambient_color: 'ambient_color',
    night_color: 'night_color',
    moon_phase: 'moon_phase',
    moon_angle: 'moon_angle',
    moon_scale: 'moon_scale',
    biome: 'biome',
    wind: 'wind',
    wind_speed: 'wind_speed',
    wind_strength: 'wind_strength',
    wind_direction: 'wind_direction',
  };

  _opBackgroundSet(id, name, args) {
    const has = (k) => Object.prototype.hasOwnProperty.call(args, k);

    if (has('sun_angle')) {
      return {
        id,
        op: name,
        ok: 0,
        error:
          'sun_angle es ambiguo y NO es la brujula: el canal que lleva ese nombre en el motor ' +
          '(background_sunlight_angle, 0.526) es el DIAMETRO ANGULAR del disco solar, o sea la ' +
          'blandura de la penumbra. Usa sun_azimuth (brujula: donde esta el sol, 0=+Y 90=+X) o ' +
          'sun_softness (penumbra).',
      };
    }
    if (has('sun_azimuth') && has('sun_rotation')) {
      return { id, op: name, ok: 0, error: 'sun_azimuth y sun_rotation son el MISMO canal (azimut = rotation + 180): manda solo uno' };
    }
    if (has('sun_elevation') && has('sun_time')) {
      return { id, op: name, ok: 0, error: 'sun_elevation y sun_time son el MISMO canal (elevacion = 90 - time): manda solo uno' };
    }

    const known = new Set([...Object.keys(FakeEditor.ENV_MAP), 'sun_azimuth', 'sun_elevation', 'ensure_track', 'frame']);
    const skipped = Object.keys(args).filter((k) => !known.has(k));

    let created = 0;
    if (has('ensure_track') && args.ensure_track) {
      if (!this.bgTrack) {
        this.bgTrack = true;
        created = 1;
      }
    }
    if (has('frame')) this.state.marker = Math.max(0, Math.round(Number(args.frame)));

    if (has('ground_block')) {
      const gb = String(args.ground_block);
      if (gb.slice(0, 6) !== 'block/') {
        return { id, op: name, ok: 0, error: `bloque de suelo desconocido: ${gb} -- parecidos: (ninguno)` };
      }
    }

    const batch = [];
    if (has('sun_azimuth')) batch.push(['sky_rotation', ((Number(args.sun_azimuth) - 180) % 360 + 360) % 360]);
    if (has('sun_elevation')) batch.push(['sky_time', 90 - Number(args.sun_elevation)]);
    for (const [k, field] of Object.entries(FakeEditor.ENV_MAP)) {
      if (has(k)) batch.push([field, args[k]]);
    }
    if (has('fog_color')) batch.push(['fog_color_custom', 1]);
    if (has('fog_object_color')) batch.push(['fog_object_color_custom', 1]);

    if (batch.length === 0) {
      return {
        id,
        op: name,
        ok: 0,
        error:
          'ningun campo aplicable' +
          (skipped.length ? ' -- desconocidos: ' + skipped.map((f) => f + ',').join('') : '') +
          ' -- validos: ' +
          [...known].join(';'),
      };
    }

    const before = { ...this.background };
    for (const [field, value] of batch) this.background[field] = value;

    let kfCreated = 0;
    if (this.bgTrack) {
      const f = this.state.marker;
      if (!this.bgKeyframes.includes(f)) {
        this.bgKeyframes.push(f);
        kfCreated = 1;
      }
    }
    this._pushHistory({ kind: 'background', before, after: null });

    // Geometria solar, calculada igual que el motor: direccion del sol =
    // (-sin R sin T, -cos R sin T, cos T) con T = sky_time, R = sky_rotation.
    const T = (Number(this.background.sky_time) * Math.PI) / 180;
    const R = (Number(this.background.sky_rotation) * Math.PI) / 180;
    const dx = -Math.sin(R) * Math.sin(T);
    const dy = -Math.cos(R) * Math.sin(T);
    const dz = Math.cos(T);
    const az = ((Math.atan2(dx, dy) * 180) / Math.PI + 360) % 360;
    const el = (Math.asin(Math.max(-1, Math.min(1, dz))) * 180) / Math.PI;
    const r2 = (v) => Math.round(v * 100) / 100;

    const out = {
      id,
      op: name,
      ok: 1,
      applied: batch.length,
      via: this.bgTrack ? 'background_object' : 'app',
      undoable: 1,
      track: this.bgTrack ? 1 : 0,
      track_created: created,
      kf_created: kfCreated,
      sun_azimuth: r2(az),
      sun_elevation: r2(el),
      shadow_len_32: el > 0.5 ? r2(32 * (Math.cos((el * Math.PI) / 180) / Math.sin((el * Math.PI) / 180))) : -1,
      shadow_azimuth: (az + 180) % 360,
      ...this.background,
    };
    if (has('frame')) out.frame = this.state.marker;
    if (skipped.length > 0) {
      out.skipped = skipped.map((f) => f + ',').join('');
      out.valid_fields = [...known].join(';');
    }
    return out;
  }

  // RONDA K1 original: historyStack.push({...}) + historyPos++ en cada sitio
  // que abre una accion. RONDA K2 lo centraliza aqui SOLO para poder limpiar
  // el redoStack en cada accion nueva (semantica estandar: una accion nueva
  // invalida el "rehacer" pendiente) sin tocar el resto del comportamiento ya
  // verificado (mismo historyStack, mismo historyPos, mismas formas de entry).
  _pushHistory(entry) {
    this.historyStack.push(entry);
    this.historyPos++;
    this.redoStack = [];
  }

  _opUndo(id, name) {
    const entry = this.historyStack.pop();
    if (!entry) return { id, op: name, ok: 0, error: 'nada que deshacer' };
    this.historyPos = Math.max(0, this.historyPos - 1);
    if (!this.redoStack) this.redoStack = [];

    // RONDA K1: entradas de historial que no son "un objeto de escena" --
    // mismo historyStack, discriminadas por "kind" (ver _opProjectSet/
    // _opMarkers/_opKeyframes/_opObjectEdit). "after" se captura AQUI (estado
    // justo antes de deshacer, que es exactamente lo que edit.redo debe
    // reproducir) para no tener que tocar cada sitio que empuja al historial.
    if (entry.kind === 'project') {
      entry.after = this.projectSettings;
      this.projectSettings = entry.before;
      this.redoStack.push(entry);
      return { id, op: name, ok: 1, target: '(project)', history_pos: this.historyPos };
    }
    if (entry.kind === 'marker') {
      entry.after = this.markers;
      this.markers = entry.before;
      this.redoStack.push(entry);
      return { id, op: name, ok: 1, target: '(marker)', history_pos: this.historyPos };
    }
    if (entry.kind === 'keyframes') {
      const o = this.findObject(entry.name);
      entry.after = o ? cloneObj(o.keyframes) : [];
      if (o) o.keyframes = entry.before;
      this.redoStack.push(entry);
      return { id, op: name, ok: 1, target: '(keyframes)', history_pos: this.historyPos };
    }
    if (entry.kind === 'objectEdit') {
      for (const w of entry.writes) {
        const t = this.scene.find((x) => x.saveId === w.saveId);
        if (t) t[w.field] = w.old;
      }
      this.redoStack.push(entry);
      return { id, op: name, ok: 1, target: '(object_edit)', history_pos: this.historyPos };
    }
    // RONDA K3: el panel de fondo/sol entero es UNA entrada (mismo criterio
    // que el op real: un cambio de fondo = un solo Ctrl+Z).
    if (entry.kind === 'background') {
      entry.after = { ...this.background };
      this.background = entry.before;
      this.redoStack.push(entry);
      return { id, op: name, ok: 1, target: '(background)', history_pos: this.historyPos };
    }
    const o = this.findObject(entry.name);
    if (o) {
      entry.after = cloneObj(o);
      Object.assign(o, entry.before);
    }
    this.redoStack.push(entry);
    return { id, op: name, ok: 1, target: entry.name, history_pos: this.historyPos };
  }

  // RONDA K2: edit.redo (Ctrl+Y), simetrico de edit.undo via redoStack (ver
  // _opUndo -- "after" queda grabado en la entrada al deshacer).
  _opRedo(id, name) {
    if (!this.redoStack) this.redoStack = [];
    const entry = this.redoStack.pop();
    if (!entry) return { id, op: name, ok: 0, error: 'nada que rehacer' };
    this.historyPos++;
    this.historyStack.push(entry);

    if (entry.kind === 'project') {
      this.projectSettings = entry.after;
      return { id, op: name, ok: 1, target: '(project)', history_pos: this.historyPos };
    }
    if (entry.kind === 'marker') {
      this.markers = entry.after;
      return { id, op: name, ok: 1, target: '(marker)', history_pos: this.historyPos };
    }
    if (entry.kind === 'keyframes') {
      const o = this.findObject(entry.name);
      if (o) o.keyframes = cloneObj(entry.after);
      return { id, op: name, ok: 1, target: '(keyframes)', history_pos: this.historyPos };
    }
    if (entry.kind === 'objectEdit') {
      for (const w of entry.writes) {
        const t = this.scene.find((x) => x.saveId === w.saveId);
        if (t) t[w.field] = w.new;
      }
      return { id, op: name, ok: 1, target: '(object_edit)', history_pos: this.historyPos };
    }
    if (entry.kind === 'background') {
      this.background = { ...entry.after };
      return { id, op: name, ok: 1, target: '(background)', history_pos: this.historyPos };
    }
    const o = this.findObject(entry.name);
    if (o) Object.assign(o, entry.after);
    return { id, op: name, ok: 1, target: entry.name, history_pos: this.historyPos };
  }

  // RONDA K1: assistant.project_get/_set + project.load_preset. Ver la
  // cabecera de PSET_FIELDS -- contrato, no fidelidad al motor real.
  _opProjectGet(id, name) {
    const fields = Object.keys(PSET_FIELDS);
    const txt = fields.map((f) => `${f}=${this.projectSettings[f]}`).join(';') + ';';
    return { id, op: name, ok: 1, fields: txt, count: fields.length };
  }

  _opProjectSet(id, name, args) {
    const settings = args.settings || {};
    const fields = Object.keys(PSET_FIELDS);
    const before = { ...this.projectSettings };
    let applied = 0;
    const skipped = [];
    for (const [k, v] of Object.entries(settings)) {
      if (!fields.includes(k)) {
        skipped.push(k);
        continue;
      }
      this.projectSettings[k] = typeof v === 'boolean' ? (v ? 1 : 0) : v;
      applied++;
    }
    if (applied === 0) {
      return {
        id,
        op: name,
        ok: 0,
        error: `ningun campo aplicable (${skipped.join(',')}) -- validos: ${fields.join(',')}`,
      };
    }
    this._pushHistory({ kind: 'project', before });
    const r = { id, op: name, ok: 1, applied, undoable: 1 };
    if (skipped.length) r.skipped = skipped.join(',');
    return r;
  }

  _opPresetList(id, name) {
    const names = Object.keys(this.presets);
    return { id, op: name, ok: 1, count: names.length, items: names.join(';') + ';' };
  }

  _opPresetLoad(id, name, args) {
    const preset = this.presets[args.name];
    if (!preset) return { id, op: name, ok: 0, error: 'preset no existe: ' + args.name };
    Object.assign(this.projectSettings, preset);
    return { id, op: name, ok: 1, engine: this.projectSettings.engine, tonemapper: this.projectSettings.tonemapper };
  }

  _opTimelineSetLength(id, name, args) {
    if (args.frames !== undefined) this.state.timelineLength = args.frames;
    return { id, op: name, ok: 1 };
  }

  // RONDA K1: assistant.markers {action, frame?, name?, color?, ref?}.
  _findMarker(ref) {
    const idx = Number(ref);
    if (Number.isInteger(idx) && idx >= 1 && idx <= this.markers.length) return this.markers[idx - 1];
    return (
      this.markers.find((m) => m.name === ref) ||
      this.markers.find((m) => m.name.toLowerCase() === String(ref).toLowerCase()) ||
      null
    );
  }

  _opMarkers(id, name, args) {
    const action = args.action;

    if (action === 'list') {
      const items = this.markers.map((m) => `${m.name}|${m.pos}|${m.color}`).join(';') + (this.markers.length ? ';' : '');
      return { id, op: name, ok: 1, count: this.markers.length, items };
    }

    if (action === 'add') {
      if (args.frame === undefined) return { id, op: name, ok: 0, error: 'falta frame' };
      const before = cloneObj(this.markers);
      const m = { name: args.name || 'New marker', pos: Math.round(args.frame), color: args.color ?? 0 };
      this.markers.push(m);
      this.markers.sort((a, b) => a.pos - b.pos);
      this._pushHistory({ kind: 'marker', before });
      return { id, op: name, ok: 1, name: m.name, pos: m.pos, color: m.color, undoable: 1 };
    }

    if (action === 'edit') {
      const m = this._findMarker(args.ref);
      if (!m) return { id, op: name, ok: 0, error: 'marcador no encontrado: ' + args.ref };
      const before = cloneObj(this.markers);
      if (args.name !== undefined) m.name = args.name;
      if (args.color !== undefined) m.color = args.color;
      if (args.frame !== undefined) m.pos = Math.round(args.frame);
      this.markers.sort((a, b) => a.pos - b.pos);
      this._pushHistory({ kind: 'marker', before });
      return { id, op: name, ok: 1, name: m.name, pos: m.pos, color: m.color, undoable: 1 };
    }

    if (action === 'delete') {
      const m = this._findMarker(args.ref);
      if (!m) return { id, op: name, ok: 0, error: 'marcador no encontrado: ' + args.ref };
      const before = cloneObj(this.markers);
      this.markers = this.markers.filter((x) => x !== m);
      this._pushHistory({ kind: 'marker', before });
      return { id, op: name, ok: 1, name: m.name, undoable: 1 };
    }

    return { id, op: name, ok: 0, error: 'action desconocida' };
  }

  // -------------------------------------------------------------------
  // RONDA K2: mi3_keyframes (assistant.keyframes) + mi3_object_edit
  // (assistant.object_edit) + edit.redo + reparentar/transform copy-paste/
  // mirror_pose/focus_on/ortho. Mismo espiritu que el resto del fichero (ver
  // cabecera): ejercita el CONTRATO de las tools nuevas -- nombres de campo,
  // "applied"/"skipped", errores, caps -- no cada quirk del motor real (eso
  // se verifica contra el editor de verdad, gates G2.1-G2.8 del spec).
  // -------------------------------------------------------------------

  _findKeyframe(o, pos) {
    return (o.keyframes || []).find((k) => k.pos === pos) || null;
  }

  _opKeyframes(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada: ' + args.ref };
    if (!o.keyframes) o.keyframes = [];
    const action = args.action;

    if (action === 'list') {
      const txt =
        o.keyframes
          .map((k, idx) => {
            const prevValues = idx > 0 ? o.keyframes[idx - 1].values || {} : {};
            const keys = idx === 0 ? Object.keys(k.values || {}) : Object.keys(k.values || {}).filter((f) => prevValues[f] !== k.values[f]);
            return `${k.pos}|${k.transition}|${keys.length}`;
          })
          .join(';') + (o.keyframes.length ? ';' : '');
      return { id, op: name, ok: 1, keyframes: txt, count: o.keyframes.length };
    }

    if (action === 'delete') {
      const frames = args.frames || [];
      if (!frames.length) return { id, op: name, ok: 0, error: 'falta frames' };
      for (const f of frames) if (!this._findKeyframe(o, f)) return { id, op: name, ok: 0, error: 'no hay keyframe en ' + f };
      const before = cloneObj(o.keyframes);
      o.keyframes = o.keyframes.filter((k) => !frames.includes(k.pos));
      this._pushHistory({ kind: 'keyframes', name: o.name, before });
      return { id, op: name, ok: 1, deleted: frames.length, count: o.keyframes.length, undoable: 1 };
    }

    if (action === 'move') {
      const frames = args.frames || [];
      if (!frames.length) return { id, op: name, ok: 0, error: 'falta frames' };
      if (args.to === undefined && args.delta === undefined) return { id, op: name, ok: 0, error: 'falta to o delta' };
      const delta = args.delta !== undefined ? args.delta : args.to - frames[0];
      for (const f of frames) if (!this._findKeyframe(o, f)) return { id, op: name, ok: 0, error: 'no hay keyframe en ' + f };
      const before = cloneObj(o.keyframes);
      for (const f of frames) this._findKeyframe(o, f).pos = Math.max(0, f + delta);
      o.keyframes.sort((a, b) => a.pos - b.pos);
      this._pushHistory({ kind: 'keyframes', name: o.name, before });
      return { id, op: name, ok: 1, moved: frames.length, delta, undoable: 1 };
    }

    if (action === 'transition') {
      const transition = args.transition;
      if (!transition) return { id, op: name, ok: 0, error: 'falta transition' };
      const frames = args.frames || [];
      if (!frames.length) return { id, op: name, ok: 0, error: 'falta frames' };
      for (const f of frames) if (!this._findKeyframe(o, f)) return { id, op: name, ok: 0, error: 'no hay keyframe en ' + f };
      const before = cloneObj(o.keyframes);
      for (const f of frames) this._findKeyframe(o, f).transition = transition;
      this._pushHistory({ kind: 'keyframes', name: o.name, before });
      return { id, op: name, ok: 1, applied: frames.length, transition, undoable: 1 };
    }

    if (action === 'copy') {
      const frames = args.frames;
      let toCopy;
      if (frames && frames.length) {
        toCopy = [];
        for (const f of frames) {
          const k = this._findKeyframe(o, f);
          if (!k) return { id, op: name, ok: 0, error: 'no hay keyframe en ' + f };
          toCopy.push(k);
        }
      } else {
        toCopy = o.keyframes;
      }
      if (!toCopy.length) return { id, op: name, ok: 0, error: 'el objeto no tiene keyframes que copiar' };
      const minPos = Math.min(...toCopy.map((k) => k.pos));
      this._kfClipboard = toCopy.map((k) => ({ relPos: k.pos - minPos, transition: k.transition, values: cloneObj(k.values || {}) }));
      return { id, op: name, ok: 1, copied: this._kfClipboard.length };
    }

    if (action === 'paste') {
      if (!this._kfClipboard || !this._kfClipboard.length)
        return { id, op: name, ok: 0, error: 'portapapeles de keyframes vacio (llama antes a action:"copy")' };
      if (args.at === undefined) return { id, op: name, ok: 0, error: 'falta at' };
      const target = args.target_ref ? this.findObject(args.target_ref) : o;
      if (!target) return { id, op: name, ok: 0, error: 'target_ref no encontrada: ' + args.target_ref };
      if (!target.keyframes) target.keyframes = [];
      const before = cloneObj(target.keyframes);
      const kfBefore = target.keyframes.length;
      const pasted = this._kfClipboard.length;
      for (const c of this._kfClipboard) {
        const pos = args.at + c.relPos;
        let k = this._findKeyframe(target, pos);
        if (!k) {
          k = { pos, transition: c.transition, values: {} };
          target.keyframes.push(k);
        }
        Object.assign(k.values, c.values);
        k.transition = c.transition;
      }
      target.keyframes.sort((a, b) => a.pos - b.pos);
      this._pushHistory({ kind: 'keyframes', name: target.name, before });
      return { id, op: name, ok: 1, target: target.name, pasted, kf_before: kfBefore, kf_after: target.keyframes.length, undoable: 1 };
    }

    return { id, op: name, ok: 0, error: 'action desconocida' };
  }

  _collectTree(o) {
    const out = [o];
    let changed = true;
    while (changed) {
      changed = false;
      for (const item of this.scene) {
        if (!out.includes(item) && out.some((p) => item.parent === p.name)) {
          out.push(item);
          changed = true;
        }
      }
    }
    return out;
  }

  _opObjectEdit(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada: ' + args.ref };
    const action = args.action;

    if (action === 'ik') {
      const hasTarget = Object.prototype.hasOwnProperty.call(args, 'target');
      const hasBlend = Object.prototype.hasOwnProperty.call(args, 'ik_blend');
      const hasAngle = Object.prototype.hasOwnProperty.call(args, 'ik_angle_offset');
      if (!hasTarget && !hasBlend && !hasAngle) return { id, op: name, ok: 0, error: 'ik: nada que aplicar' };
      const applied = [];
      if (hasTarget) {
        if (args.target) {
          const t = this.findObject(args.target);
          if (!t) return { id, op: name, ok: 0, error: 'target no encontrado: ' + args.target };
          o.ikTarget = t.name;
        } else {
          o.ikTarget = null;
        }
        applied.push('target');
      }
      if (hasBlend) {
        o.ikBlend = args.ik_blend;
        applied.push('ik_blend');
      }
      if (hasAngle) {
        o.ikAngleOffset = args.ik_angle_offset;
        applied.push('ik_angle_offset');
      }
      return {
        id,
        op: name,
        ok: 1,
        applied: applied.join(',') + ',',
        ik_blend: o.ikBlend ?? 0,
        ik_angle_offset: o.ikAngleOffset ?? 0,
        undoable: 1,
      };
    }

    // Lote generico: [target, field, value][] -- precedente de forma:
    // action_assistant_object_edit (una sola entrada de historial).
    const writes = [];

    if (action === 'rename') {
      if (!args.name) return { id, op: name, ok: 0, error: 'falta name' };
      writes.push([o, 'name', args.name]);
    } else if (action === 'hide') {
      if (args.hidden === undefined) return { id, op: name, ok: 0, error: 'falta hidden' };
      for (const t of this._collectTree(o)) writes.push([t, 'hide', args.hidden ? 1 : 0]);
    } else if (action === 'appearance') {
      const app = args.appearance;
      if (!app || Object.keys(app).length === 0) return { id, op: name, ok: 0, error: 'falta appearance' };
      for (const t of this._collectTree(o)) {
        for (const k of ['backfaces', 'shadows', 'ssao', 'glow', 'fog'])
          if (app[k] !== undefined) writes.push([t, k, app[k] ? 1 : 0]);
        if (app.blend_mode !== undefined) writes.push([t, 'blendMode', app.blend_mode]);
        if (app.alpha_mode !== undefined) writes.push([t, 'alphaMode', app.alpha_mode]);
      }
    } else if (action === 'inherit') {
      const inh = args.inherit;
      const validKeys = [
        'position', 'rotation', 'scale', 'bend', 'pose', 'rot_point',
        'texture', 'color', 'alpha', 'glow_color', 'subsurface', 'surface',
        'visibility', 'select',
      ];
      if (!inh || Object.keys(inh).length === 0) return { id, op: name, ok: 0, error: 'falta inherit' };
      const skipped = [];
      for (const [k, v] of Object.entries(inh)) {
        if (!validKeys.includes(k)) {
          skipped.push(k);
          continue;
        }
        writes.push([o, 'inherit_' + k, v ? 1 : 0]);
      }
      if (writes.length === 0)
        return { id, op: name, ok: 0, error: 'ningun campo valido en inherit' + (skipped.length ? ` (desconocidos: ${skipped.join(',')})` : '') };
    } else if (action === 'color_tag') {
      if (args.color_tag === undefined) return { id, op: name, ok: 0, error: 'falta color_tag' };
      writes.push([o, 'colorTag', args.color_tag]);
    } else if (action === 'rot_point') {
      const rp = args.rot_point;
      if (!rp || rp.length !== 3) return { id, op: name, ok: 0, error: 'falta rot_point [x,y,z]' };
      writes.push([o, 'rotPoint', [...rp]]);
    } else {
      return { id, op: name, ok: 0, error: 'action desconocida: ' + action };
    }

    const entryWrites = writes.map(([t, field, value]) => ({ saveId: t.saveId, field, old: t[field], new: value }));
    for (const [t, field, value] of writes) t[field] = value;
    this._pushHistory({ kind: 'objectEdit', writes: entryWrites });

    return { id, op: name, ok: 1, applied: writes.length, undoable: 1 };
  }

  // ShinyTrims O6: armor.set_trim {target, piece?, pattern?, material?,
  // effect?, intensity?} -- simulacion minima, solo para probar el CONTRATO
  // (que la tool manda el schema correcto, no la semantica real del motor
  // de plantilla/set-canon).
  _opSetTrim(id, name, args) {
    const o = this.findObject(args.target);
    if (!o) return { id, op: name, ok: 0, error: 'target no encontrado: ' + args.target };
    if (args.pattern !== undefined) o.trimPattern = args.pattern;
    if (args.material !== undefined) o.trimMaterial = args.material;
    if (args.effect !== undefined) o.trimEffect = args.effect;
    if (args.intensity !== undefined) o.trimIntensity = args.intensity;
    return {
      id,
      op: name,
      ok: 1,
      target: args.target,
      piece: args.piece ?? 'all',
      trim_pattern: o.trimPattern ?? '',
      trim_material: o.trimMaterial ?? '',
      trim_fx: o.trimEffect ?? 'none',
      undoable: 1,
    };
  }

  // ShinyTrims O7: item.set_trim / item.get_trim -- simulacion minima, solo
  // para probar el CONTRATO de la tool (que manda el op correcto con el schema
  // correcto), no la composicion real del PNG, que vive en el motor y se mide
  // con la bateria de gates GT1-GT8, no aqui.
  _opSetItemTrim(id, name, args) {
    const o = this.findObject(args.target);
    if (!o) return { id, op: name, ok: 0, error: 'target no encontrado: ' + args.target };
    if (args.pattern !== undefined) o.itemTrimPattern = args.pattern;
    if (args.material !== undefined) o.itemTrimMaterial = args.material;
    if (args.effect !== undefined) o.itemTrimEffect = args.effect;
    if (args.intensity !== undefined) o.itemTrimIntensity = args.intensity;
    return {
      id,
      op: name,
      ok: 1,
      target: args.target,
      trim_pattern: o.itemTrimPattern ?? 'none',
      trim_material: o.itemTrimMaterial ?? '',
      trim_fx_name: o.itemTrimEffect ?? 'none',
      undoable: 1,
      undo_covers: 'trim_pattern,trim_material,trim_fx,trim_intensity,item_tex,item_tex_material,trim_palette',
      undo_misses: '',
    };
  }

  _opGetItemTrim(id, name, args) {
    const o = this.findObject(args.target);
    if (!o) return { id, op: name, ok: 0, error: 'target no encontrado: ' + args.target };
    return {
      id,
      op: name,
      ok: 1,
      target: args.target,
      trim_pattern: o.itemTrimPattern ?? 'none',
      trim_material: o.itemTrimMaterial ?? '',
      trim_fx_name: o.itemTrimEffect ?? 'none',
    };
  }

  _opParent(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada' };
    const before = o.parent;
    if (args.parent) {
      const par = this.findObject(args.parent);
      if (!par) return { id, op: name, ok: 0, error: 'parent no encontrada: ' + args.parent };
      o.parent = par.name;
    } else {
      o.parent = '';
    }
    this._pushHistory({ kind: 'objectEdit', writes: [{ saveId: o.saveId, field: 'parent', old: before, new: o.parent }] });
    return { id, op: name, ok: 1, name: o.name, parent: o.parent, undoable: 1 };
  }

  _opTransformCopy(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada: ' + args.ref };
    this._tfClipboard = { hasPos: 1, hasRot: 1, hasSca: o.type !== 'camera' ? 1 : 0, pos: [...o.pos], rot: [...o.rot], sca: o.sca ? [...o.sca] : [1, 1, 1], src: o.name };
    return {
      id,
      op: name,
      ok: 1,
      source: o.name,
      has_pos: this._tfClipboard.hasPos,
      has_rot: this._tfClipboard.hasRot,
      has_sca: this._tfClipboard.hasSca,
      pos: this._tfClipboard.pos.join(','),
      rot: this._tfClipboard.rot.join(','),
      sca: this._tfClipboard.sca.join(','),
    };
  }

  _opTransformPaste(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada: ' + args.ref };
    if (!this._tfClipboard) return { id, op: name, ok: 0, error: 'portapapeles de transform vacio (falta transform.copy)' };
    const before = cloneObj(o);
    let appliedN = 0;
    const skipped = [];
    if (this._tfClipboard.hasPos) {
      o.pos = [...this._tfClipboard.pos];
      appliedN += 3;
    } else skipped.push('POS');
    o.rot = [...this._tfClipboard.rot];
    appliedN += 3;
    if (this._tfClipboard.hasSca && o.type !== 'camera') {
      o.sca = [...this._tfClipboard.sca];
      appliedN += 3;
    } else skipped.push('SCA');
    const kfBefore = o.kf;
    o.kf = o.kf + 1;
    this._pushHistory({ name: o.name, before });
    return {
      id,
      op: name,
      ok: 1,
      target: o.name,
      applied: appliedN,
      skipped: skipped.join(','),
      kf_before: kfBefore,
      kf_after: o.kf,
      kf_created: 1,
      marker: this.state.marker,
    };
  }

  _opMirrorPose(id, name, args) {
    const from = this.findObject(args.from);
    const to = this.findObject(args.to);
    if (!from || !to) return { id, op: name, ok: 0, error: 'from o to no encontrado' };
    to.rot = [...from.rot];
    return { id, op: name, ok: 1, from: from.name, to: to.name, matched: 1 };
  }

  _opFocusOn(id, name, args) {
    const cam = this.findObject(args.ref);
    const target = this.findObject(args.target);
    if (!cam) return { id, op: name, ok: 0, error: 'ref no encontrada' };
    if (!target) return { id, op: name, ok: 0, error: 'target no encontrado' };
    const dx = target.pos[0] - cam.pos[0];
    const dy = target.pos[1] - cam.pos[1];
    const dz = target.pos[2] - cam.pos[2];
    cam.dofDepth = Math.sqrt(dx * dx + dy * dy + dz * dz);
    return { id, op: name, ok: 1, camera: cam.name, target: target.name, distance: cam.dofDepth };
  }

  _opOrthoToggle(id, name, args) {
    const cam = this.findObject(args.ref);
    if (!cam) return { id, op: name, ok: 0, error: 'ref no encontrada' };
    if (cam.type !== 'camera') return { id, op: name, ok: 0, error: 'ref no es una CAMARA' };
    const before = cam.orthoScale ?? 128;
    cam.ortho = (args.enable ?? 1) != 0;
    cam.orthoScale = cam.ortho ? before * 2 : before; // escala arbitraria distinta de before, solo para el contrato
    return { id, op: name, ok: 1, ortho: String(cam.ortho ? 1 : 0), scale_before: String(before), scale: String(cam.orthoScale) };
  }

  // RONDA F: assistant.object_add / assistant.object_delete / object.duplicate
  // (via:"ui"). Minimal in-memory stand-in (see file header) -- crear
  // devuelve save_id nuevo + name; borrar quita el objeto y todo lo que
  // cuelgue de el (por parent-por-nombre, igual que _opObjectGet lo lee).
  // No modela historial/undo (eso se prueba contra el editor REAL, gate F3);
  // aqui solo se ejercita el CONTRATO del op para las tools de tools.ts.
  _nextSaveId() {
    if (this._saveIdCounter === undefined) {
      this._saveIdCounter = 1 + this.scene.reduce((m, o) => Math.max(m, Number(o.saveId) || 0), 0);
    }
    return this._saveIdCounter++;
  }

  _opObjectAdd(id, name, args) {
    const kind = args.kind;
    const nm = args.name;
    if (!nm) return { id, op: name, ok: 0, error: 'falta name' };

    let type;
    switch (kind) {
      case 'character':
        type = 'character';
        break;
      case 'camera':
        type = 'camera';
        break;
      case 'light':
        type = args.light === 'spot' ? 'spot_light' : 'point_light';
        break;
      case 'folder':
        type = 'folder';
        break;
      case 'primitive':
        type = String(args.shape || 'CUBE').toLowerCase();
        break;
      case 'item':
        type = 'item';
        break;
      case 'block':
        type = 'block';
        break;
      // RONDA K4
      case 'armor':
        if (!args.material) return { id, op: name, ok: 0, error: 'falta material' };
        type = 'model';
        break;
      case 'text':
        if (!args.text) return { id, op: name, ok: 0, error: 'falta text' };
        type = 'text';
        break;
      case 'model':
      case 'particles':
        if (!args.path) return { id, op: name, ok: 0, error: 'falta path' };
        type = kind === 'model' ? 'model' : 'particle_spawner';
        break;
      default:
        return { id, op: name, ok: 0, error: 'kind desconocido: ' + kind };
    }

    const saveId = this._nextSaveId();
    const obj = {
      saveId,
      name: nm,
      type,
      parent: '',
      pos: [args.pos_x ?? 0, args.pos_y ?? 0, args.pos_z ?? 0],
      rot: [args.rot_x ?? 0, args.rot_y ?? 0, args.rot_z ?? 0],
      sca: [1, 1, 1],
      kf: 0,
      keyframes: [],
    };
    if (kind === 'camera') Object.assign(obj, { fov: 45, ortho: false, active: false, hide: false, lightManagement: 1 });
    if (kind === 'light') Object.assign(obj, { strength: 1, range: 20 });
    if (kind === 'text') Object.assign(obj, { text: args.text, align: args.align ?? 'center', outline: !!args.outline });
    if (kind === 'armor') Object.assign(obj, { material: args.material });

    if (kind === 'armor' && !args.stand && args.wear_on) {
      const target = this.findObject(args.wear_on);
      if (!target) return { id, op: name, ok: 0, error: 'wear_on no encontrado: ' + args.wear_on };
      obj.parent = target.name;
    } else if (args.parent) {
      const par = this.findObject(args.parent);
      if (par) obj.parent = par.name;
    }

    this.scene.push(obj);

    const result = { id, op: name, ok: 1, save_id: String(saveId), name: nm, kind, undoable: 1, created_roots: 1 };
    if (kind === 'armor' && !args.stand && args.wear_on) result.mirror_pose_parts_matched = 6;
    return result;
  }

  // RONDA K4 --------------------------------------------------------------

  /** RONDA K5: project.open {path}. */
  _opProjectOpen(id, name, args) {
    const p = args && args.path;
    if (!p || typeof p !== 'string') return { id, op: name, ok: 0, error: 'path no existe: ' + p };
    this.state.projectFile = path.basename(p);
    this.state.projectFolder = path.dirname(p);
    this.state.projectSaved = true;
    this.historyStack = [];
    this.historyPos = 0;
    this.lastOpenedPath = p;
    return { id, op: name, ok: 1, objects: this.scene.length };
  }

  /** RONDA K5: project.new {name?}. */
  _opProjectNew(id, name, args) {
    const nm = args && typeof args.name === 'string' && args.name ? args.name : 'hl_project';
    this.scene = initialScene();
    this.state.projectFile = `${nm}.miproject`;
    this.state.projectFolder = path.join(this.state.projectsFolder, nm);
    this.state.projectSaved = true;
    this.historyStack = [];
    this.historyPos = 0;
    return { id, op: name, ok: 1, objects: this.scene.length, project_folder: this.state.projectFolder };
  }

  /** RONDA K5: project.save {path?} -- sin path, guarda encima del fichero
   *  actual; con path, es el "save as" (D16: avisa si la carpeta cambia). */
  _opProjectSave(id, name, args) {
    let assetsLeft = 0;
    if (args && typeof args.path === 'string' && args.path) {
      const newFolder = path.dirname(args.path);
      if (this.state.projectFolder && newFolder !== this.state.projectFolder) assetsLeft = 1;
      this.state.projectFile = path.basename(args.path);
      this.state.projectFolder = newFolder;
      this.lastSavedPath = args.path;
    }
    this.state.projectSaved = true;
    this.historyStack = [];
    this.historyPos = 0;
    return { id, op: name, ok: 1, objects: this.scene.length, assets_left_behind: assetsLeft };
  }

  /** RONDA K5: project.checkpoint_restore {ref} -- ref es id O nombre, busca
   *  en AMBOS pools (auto y manual), igual que mi3_checkpoint_find real. */
  _opProjectCheckpointRestore(id, name, args) {
    const ref = args && args.ref;
    if (!ref) return { id, op: name, ok: 0, error: 'falta ref' };
    const all = [...this.autoCheckpoints, ...this.manualCheckpoints];
    const cp = all.find((c) => c.id === ref || c.name === ref);
    if (!cp) return { id, op: name, ok: 0, error: 'checkpoint no encontrado: ' + ref };
    this.scene = cloneObj(cp.snapshot);
    this.historyStack = [];
    this.historyPos = 0;
    return { id, op: name, ok: 1, cp_id: cp.id, cp_name: cp.name, objects: this.scene.length };
  }

  _opPaths(id, name, _args) {
    return {
      id,
      op: name,
      ok: 1,
      projects_folder: this.state.projectsFolder,
      project_folder: this.state.projectFolder,
      project_file: this.state.projectFile,
    };
  }

  _opLibrary(id, name, args) {
    const kind = args.kind;
    if (kind === 'sticker_packs') {
      const names = Object.keys(this.stickerPacks);
      return { id, op: name, ok: 1, count: names.length, entries: names.map((n) => `${n};`).join('') };
    }
    if (kind === 'sticker_items') {
      const pack = args.pack;
      if (!pack || !this.stickerPacks[pack]) return { id, op: name, ok: 0, error: 'pack desconocido: ' + pack };
      const items = this.stickerPacks[pack];
      return { id, op: name, ok: 1, count: items.length, entries: items.map((n) => `${n};`).join(''), pack };
    }
    const known = ['projects', 'project_assets', 'particles', 'renders', 'attachments'];
    if (!known.includes(kind)) return { id, op: name, ok: 0, error: 'kind desconocido: ' + kind };
    // Sin motor real detras: siempre vacio pero VALIDO (count=0).
    return { id, op: name, ok: 1, count: 0, entries: '' };
  }

  _opStickerAdd(id, name, args) {
    const pack = args.pack;
    const item = args.item;
    if (!pack || !this.stickerPacks[pack]) return { id, op: name, ok: 0, error: 'pack desconocido: ' + pack };
    if (!this.stickerPacks[pack].includes(item)) return { id, op: name, ok: 0, error: 'item desconocido: ' + item };
    const saveId = this._nextSaveId();
    const nm = args.name || item;
    this.scene.push({ saveId, name: nm, type: 'item', parent: '', pos: [0, 0, 0], rot: [0, 0, 0], sca: [1, 1, 1], kf: 0, keyframes: [] });
    return { id, op: name, ok: 1, save_id: String(saveId), name: nm, pack, item, undoable: 1 };
  }

  _opAssetImport(id, name, args) {
    if (!args.path) return { id, op: name, ok: 0, error: 'path no existe: ' + args.path };
    const saveId = this._nextSaveId();
    const nm = args.name || 'ImportedAsset';
    this.scene.push({ saveId, name: nm, type: 'model', parent: '', pos: [0, 0, 0], rot: [0, 0, 0], sca: [1, 1, 1], kf: 0, keyframes: [] });
    return { id, op: name, ok: 1, save_id: String(saveId), name: nm, added: 1, path: args.path };
  }

  _opResImport(id, name, args) {
    if (!args.path) return { id, op: name, ok: 0, error: 'path no existe: ' + args.path };
    const saveId = this._nextSaveId();
    return { id, op: name, ok: 1, save_id: String(saveId), filename: 'fake_res_' + saveId, type: args.type };
  }

  _opSetSkin(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada' };
    return { id, op: name, ok: 1, save_id: args.res, filename: 'fake_skin_' + args.res };
  }

  /** RESCATE K4: import + aplicacion de skin en UNA accion deshacible
   *  (assistant.skin_apply). Sustituye al par res.import + object.set_skin. */
  _opSkinApply(id, name, args) {
    if (!args.path) return { id, op: name, ok: 0, error: 'path no existe: ' + args.path };
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada: ' + args.ref };
    const saveId = this._nextSaveId();
    o.skin = String(saveId);
    return {
      id,
      op: name,
      ok: 1,
      save_id: String(saveId),
      filename: 'fake_res_' + saveId,
      applied_to: o.name,
      undoable: 1,
    };
  }

  _opWorldImport(id, name, args) {
    if (!args.path) return { id, op: name, ok: 0, error: 'falta path' };
    const saveId = this._nextSaveId();
    this.scene.push({ saveId, name: 'FakeScenery', type: 'scenery', parent: '', pos: [args.x ?? 0, args.y ?? 0, args.z ?? 0], rot: [0, 0, 0], sca: [1, 1, 1], kf: 0, keyframes: [] });
    return { id, op: name, ok: 1, mode: 'world', objects_created: 1, save_id: String(saveId) };
  }

  _opObjectDelete(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada' };

    const toDelete = new Set([o.name]);
    let changed = true;
    while (changed) {
      changed = false;
      for (const item of this.scene) {
        if (!toDelete.has(item.name) && toDelete.has(item.parent)) {
          toDelete.add(item.name);
          changed = true;
        }
      }
    }

    const before = this.scene.length;
    this.scene = this.scene.filter((item) => !toDelete.has(item.name));
    const destroyed = before - this.scene.length;
    if (this.selected && toDelete.has(this.selected.name)) this.selected = null;

    return { id, op: name, ok: 1, name: o.name, destroyed, undoable: 1 };
  }

  _opObjectDuplicate(id, name, args) {
    const o = this.findObject(args.ref);
    if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada' };

    const saveId = this._nextSaveId();
    const copy = cloneObj(o);
    copy.saveId = saveId;

    let nm = args.name;
    if (!nm) {
      const base = `${o.name} copy`;
      nm = base;
      let n = 2;
      while (this.scene.some((x) => x.name === nm)) {
        nm = `${base} ${n}`;
        n++;
      }
    }
    copy.name = nm;
    this.scene.push(copy);

    return { id, op: name, ok: 1, save_id: String(saveId), name: nm };
  }

  _opCameraPreview(id, name, args) {
    let { width = 480, height = 270, samples = 1, out } = args;
    if (!out || !out.startsWith('preview/')) return { id, op: name, ok: 0, error: 'out fuera de preview/' };
    let clamped = 0;
    if (width > 960) {
      width = 960;
      clamped = 1;
    }
    if (height > 540) {
      height = 540;
      clamped = 1;
    }
    if (samples > 4) {
      samples = 4;
      clamped = 1;
    }
    const cam = args.ref ? this.findObject(args.ref) : this.scene.find((o) => o.type === 'camera');

    // Snapshot + mutate + restore, mirroring the save/restore mi3_hl_render_op
    // needs around it (spec 4.5 / quirk Q3) so a test can assert the scene
    // comes back byte-identical afterward (gate G1.8's intent).
    const camSnapshot = this.scene
      .filter((o) => o.type === 'camera')
      .map((c) => ({ name: c.name, hide: c.hide, lm: c.lightManagement }));
    for (const c of this.scene) if (c.type === 'camera') c.hide = true;
    const savedSamples = this.state.renderSamples;
    this.state.renderSamples = -1;

    const filePath = path.join(this.dir, out);
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, makeSolidPng(width, height, 90, 140, 200));

    for (const snap of camSnapshot) {
      const c = this.scene.find((o) => o.name === snap.name);
      if (c) {
        c.hide = snap.hide;
        c.lightManagement = snap.lm;
      }
    }
    this.state.renderSamples = savedSamples;

    return { id, op: name, ok: 1, out, camera: cam ? cam.name : '(activa)', w: width, h: height, clamped };
  }

  // RONDA B: assistant.render_snapshot {job_id, camera?}. Mock de nivel 1/2 --
  // solo comprueba que el contrato del op se respeta (job_id valido, error
  // "nofolder" cuando el proyecto no esta guardado, forma del manifiesto) con
  // un install_dir que A PROPOSITO no existe: el render REAL se prueba contra
  // el editor de verdad en el gate B1, no aqui.
  _opRenderSnapshot(id, name, args) {
    const jobId = String(args.job_id ?? '');
    if (!/^[A-Za-z0-9_-]{1,48}$/.test(jobId)) return { id, op: name, ok: 0, error: `job_id invalido: ${jobId}` };
    if (!this.state.projectSaved) return { id, op: name, ok: 0, error: 'nofolder' };

    const rel = `render_job/${jobId}`;
    const dir = path.join(this.dir, rel);
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'scene.miproject'), '{"fake":1}', 'utf8');

    const projectFolder = path.join(this.dir, '_fake_project');
    const cam = args.camera ? this.findObject(args.camera) : this.scene.find((o) => o.type === 'camera');
    return {
      id,
      op: name,
      ok: 1,
      job_id: jobId,
      job_dir: rel,
      scene: `${rel}/scene.miproject`,
      project_folder: projectFolder,
      project_file: path.join(projectFolder, this.state.projectFile),
      project_name: this.state.projectFile,
      install_dir: path.join(this.dir, '_no_install'),
      marker: this.state.marker,
      engine: 2,
      samples: this.state.renderSamples,
      camera: cam ? cam.name : 'Camara',
      objects: this.scene.length,
      bytes: 9,
    };
  }

  _opCheckpoint(id, name, args) {
    if (args.action === 'create') {
      if (!this.state.projectSaved) return { id, op: name, ok: 0, error: 'nofolder' };
      const cpId = 'cp' + String(this._cpCounter++).padStart(4, '0');
      const cpName = args.name || 'assistant ' + new Date().toISOString();
      // RONDA K5: se guarda tambien un snapshot de la escena (como ya hacian
      // los checkpoints AUTOMATICOS) -- lo necesita project.checkpoint_restore
      // (mi3_project_file op:"checkpoint_restore", gate G5.6) para poder
      // restaurar algo de verdad.
      this.manualCheckpoints.push({ id: cpId, name: cpName, note: args.note || '', snapshot: cloneObj(this.scene) });
      return { id, op: name, ok: 1, cp_id: cpId, name: cpName };
    }
    if (args.action === 'list') {
      const all = [...this.manualCheckpoints, ...this.autoCheckpoints].map((c) => `${c.id}|${c.name}`).join(';');
      return {
        id,
        op: name,
        ok: 1,
        count_auto: this.autoCheckpoints.length,
        count_manual: this.manualCheckpoints.length,
        list: all,
      };
    }
    return { id, op: name, ok: 0, error: 'action desconocida' };
  }

  // RONDA C: assistant.screen_layout {camera?, w=960, h=540, refs?}. This is a
  // DELIBERATELY simplified stand-in (see file header) -- not the real
  // renderer's lookat/perspective math, just enough deterministic geometry to
  // exercise the TOOL CONTRACT end to end (field shapes, percentages, the
  // camera/refs error paths) without depending on the parallel GmProject op.
  // The real projection is verified against the actual engine separately.
  _opScreenLayout(id, name, args) {
    const w = args.w ?? 960;
    const h = args.h ?? 540;
    let cam;
    if (args.camera) {
      cam = this.findObject(args.camera);
      if (!cam) return { id, op: name, ok: 0, error: 'camara no encontrada: ' + args.camera };
      if (cam.type !== 'camera') return { id, op: name, ok: 0, error: 'la ref no es una camara: ' + args.camera };
    } else {
      cam = this.scene.find((o) => o.type === 'camera' && o.active);
      if (!cam) return { id, op: name, ok: 0, error: 'no hay camara activa en la escena; pasa camera=' };
    }

    let targets;
    if (args.refs) {
      targets = [];
      for (const raw of String(args.refs).split(',')) {
        const rf = raw.trim();
        if (!rf) continue;
        const o = this.findObject(rf);
        if (!o) return { id, op: name, ok: 0, error: 'ref no encontrada: ' + rf };
        targets.push(o);
      }
    } else {
      targets = this.scene.filter((o) => !o.parent);
    }
    if (targets.length === 0) return { id, op: name, ok: 0, error: 'sin objetos que proyectar' };

    const scale = 4; // px por unidad de mundo, fijo y arbitrario (no es la lente real)
    const project = (x, y, z) => [w / 2 + (x - cam.pos[0]) * scale, h / 2 - (z - cam.pos[2]) * scale];

    const lines = [];
    for (const o of targets) {
      const half = o.type === 'cube' ? 8 : 0; // misma caja [-8..8] que tl_bounds_local() da a un CUBE real
      let bboxTxt = '';
      let pctw = 0;
      let pcth = 0;
      let cxpct = -1;
      let cypct = -1;
      let clipped = 0;
      if (half > 0) {
        const [ax, ay] = project(o.pos[0] - half, o.pos[1] - half, o.pos[2] + half);
        const [bx, by] = project(o.pos[0] + half, o.pos[1] + half, o.pos[2] - half);
        const x0 = Math.min(ax, bx);
        const x1 = Math.max(ax, bx);
        const y0 = Math.min(ay, by);
        const y1 = Math.max(ay, by);
        bboxTxt = `${x0},${y0},${x1},${y1}`;
        pctw = ((x1 - x0) / w) * 100;
        pcth = ((y1 - y0) / h) * 100;
        cxpct = (((x0 + x1) / 2) / w) * 100;
        cypct = (((y0 + y1) / 2) / h) * 100;
        clipped = x0 < 0 || y0 < 0 || x1 > w || y1 > h ? 1 : 0;
      }
      const [px, py] = project(o.pos[0], o.pos[1], o.pos[2]);
      lines.push(
        `${o.saveId}|${o.name}|${o.type}|${bboxTxt}|${pctw},${pcth}|${cxpct},${cypct}|${px},${py}|0|0|${clipped}`,
      );
    }

    return {
      id,
      op: name,
      ok: 1,
      camera: cam.name,
      w,
      h,
      fov: cam.fov ?? 45,
      ortho: cam.ortho ? 1 : 0,
      count: targets.length,
      objects: lines.join(';'),
    };
  }

  _opPing(id, name) {
    return {
      id,
      op: name,
      ok: 1,
      busy: this.state.busy ? 1 : 0,
      popup: this.state.popup ? 1 : 0,
      playing: 0,
      changed: this.historyStack.length > 0 ? 1 : 0,
      marker: this.state.marker,
      objects: this.scene.length,
      project_saved: this.state.projectSaved ? 1 : 0,
      history_pos: this.historyPos,
      history_amount: this.historyStack.length,
    };
  }

  // -------------------------------------------------------------------
  // brain.json heartbeat consumer (section 3.6)
  // -------------------------------------------------------------------

  _pollHeartbeat() {
    const p = path.join(this.dir, 'brain.json');
    if (!fs.existsSync(p)) {
      this.conn = 'off';
      return;
    }
    let obj;
    try {
      obj = JSON.parse(stripBom(fs.readFileSync(p, 'utf8')));
    } catch {
      return;
    }
    if (obj.beat !== this.beatSeen) {
      this.beatSeen = obj.beat;
      this.beatMisses = 0;
      this.conn = 'ready';
    } else {
      this.beatMisses++;
      if (this.beatMisses >= 4) this.conn = 'gone';
    }
  }

  // -------------------------------------------------------------------
  // Test helpers
  // -------------------------------------------------------------------

  getLog() {
    return this.receivedOut;
  }

  getObject(nameOrRef) {
    return this.findObject(nameOrRef);
  }
}
