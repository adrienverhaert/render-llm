// ASSISTANT/src/projectFile.ts
//
// RONDA K5 (spec seccion 3): el guardarrail conversacional de DOS TURNOS para
// mi3_project_file op:"open"/"new"/"checkpoint_restore"/"save_as" -- las
// unicas operaciones capaces de perder trabajo del usuario.
//
// Mecanica (spec 3.1): si el proyecto tiene cambios sin guardar, la PRIMERA
// llamada registra una confirmacion PENDIENTE (op+target+turno) y rechaza sin
// tocar nada. La confirmacion solo se acepta si llega en un turno POSTERIOR
// al que la registro -- un modelo que reintente `confirm:"discard"` dentro
// del MISMO turno recibe otro rechazo. Eso es lo que hace el guardarrail
// imposible de saltar desde el prompt: el usuario tiene que haber escrito
// algo, de verdad, entre las dos llamadas.
//
// Persistencia: igual que gap.ts/memory.ts, en disco (UTF-8 sin BOM,
// escritura .part + rename) para sobrevivir a un relanzamiento del proceso
// del cerebro entre los dos turnos. `turnOrdinal` es un contador MONOTONO
// que index.ts sube una vez por turno de usuario (los turn_id del protocolo
// son opacos -- no sirven para decir "turno N+1"); vive en el MISMO fichero
// para que un reinicio no lo resetee a 0 y reabra la ventana de "mismo turno".

import fs from 'node:fs';
import path from 'node:path';

export type GuardOp = 'open' | 'new' | 'checkpoint_restore' | 'save_as';
export type GuardConfirm = 'save_first' | 'discard';

interface PendingRecord {
  op: GuardOp;
  target: string;
  turnOrdinal: number;
  turnId: string;
  /** project_file (ruta completa, "" si nunca se guardo) EN EL MOMENTO del registro --
   *  si cambia antes de la confirmacion, la espera caduca (spec 3.1: "o al cambiar de proyecto"). */
  projectFileAtRegister: string;
  createdAt: number;
}

/** spec 3.1: "pending caduca a los 3 turnos". */
const PENDING_EXPIRE_TURNS = 3;

export type ConfirmCheck = { ok: true } | { ok: false; reason: string };

export class ProjectFileGuard {
  private readonly file: string;
  private readonly logFn: (m: string) => void;
  private pending: PendingRecord | null = null;
  private turnOrdinal = 0;

  constructor(dataDir: string, logFn: (m: string) => void) {
    this.file = path.join(path.resolve(dataDir), 'project_guard.json');
    this.logFn = logFn;
    this.load();
  }

  private load(): void {
    try {
      const raw = fs.readFileSync(this.file, 'utf8');
      const obj = JSON.parse(raw) as { turnOrdinal?: unknown; pending?: unknown };
      if (typeof obj.turnOrdinal === 'number' && Number.isFinite(obj.turnOrdinal)) {
        this.turnOrdinal = obj.turnOrdinal;
      }
      if (obj.pending && typeof obj.pending === 'object') {
        const p = obj.pending as Partial<PendingRecord>;
        if (
          typeof p.op === 'string' &&
          typeof p.target === 'string' &&
          typeof p.turnOrdinal === 'number' &&
          typeof p.turnId === 'string'
        ) {
          this.pending = {
            op: p.op as GuardOp,
            target: p.target,
            turnOrdinal: p.turnOrdinal,
            turnId: p.turnId,
            projectFileAtRegister: typeof p.projectFileAtRegister === 'string' ? p.projectFileAtRegister : '',
            createdAt: typeof p.createdAt === 'number' ? p.createdAt : Date.now(),
          };
        }
      }
    } catch {
      // fichero ausente o corrupto: arranca en blanco, nunca lanza.
    }
  }

  private persist(): void {
    try {
      const tmp = this.file + '.part';
      fs.mkdirSync(path.dirname(this.file), { recursive: true });
      fs.writeFileSync(tmp, JSON.stringify({ turnOrdinal: this.turnOrdinal, pending: this.pending }), {
        encoding: 'utf8',
      });
      fs.renameSync(tmp, this.file);
    } catch (e) {
      this.logFn(`WARN project_guard: no se pudo persistir ${this.file}: ${String(e)}`);
    }
  }

  /** Llamado UNA vez por turno de usuario, ANTES de construir el TurnCtx del turno
   *  (index.ts). Devuelve el ordinal de ESTE turno. */
  nextTurn(): number {
    this.turnOrdinal++;
    this.persist();
    return this.turnOrdinal;
  }

  get currentTurnOrdinal(): number {
    return this.turnOrdinal;
  }

  /** Registra una confirmacion pendiente para (op,target) en el turno actual. */
  register(op: GuardOp, target: string, turnId: string, turnOrdinal: number, projectFile: string): void {
    this.pending = { op, target, turnOrdinal, turnId, projectFileAtRegister: projectFile, createdAt: Date.now() };
    this.persist();
    this.logFn(`RONDA K5: guardarrail registrado op=${op} target="${target}" turno=${turnId} (#${turnOrdinal})`);
  }

  /**
   * true solo si hay una espera viva para EXACTAMENTE (op,target), registrada
   * en un turno ESTRICTAMENTE ANTERIOR al actual, y el proyecto abierto sigue
   * siendo el mismo que cuando se registro. No consume el pending -- eso lo
   * hace el llamador con clear() tras ejecutar la operacion real.
   */
  checkConfirm(op: GuardOp, target: string, turnOrdinal: number, currentProjectFile: string): ConfirmCheck {
    this.expireIfStale(turnOrdinal, currentProjectFile);
    if (!this.pending) {
      return { ok: false, reason: 'no hay ninguna confirmacion pendiente (o ya caduco); vuelve a pedir la operacion primero' };
    }
    if (this.pending.op !== op || this.pending.target !== target) {
      return {
        ok: false,
        reason: `la confirmacion pendiente es para ${this.pending.op} "${this.pending.target}", no para ${op} "${target}"`,
      };
    }
    if (this.pending.turnOrdinal >= turnOrdinal) {
      return {
        ok: false,
        reason:
          'esa confirmacion tiene que venir del usuario, en el chat, en un turno POSTERIOR a cuando se pidio -- ' +
          'termina tu turno actual y espera su respuesta antes de volver a llamarme',
      };
    }
    return { ok: true };
  }

  clear(): void {
    this.pending = null;
    this.persist();
  }

  hasPending(): boolean {
    return this.pending !== null;
  }

  private expireIfStale(turnOrdinal: number, currentProjectFile: string): void {
    if (!this.pending) return;
    const tooOld = turnOrdinal - this.pending.turnOrdinal > PENDING_EXPIRE_TURNS;
    const projectChanged = this.pending.projectFileAtRegister !== currentProjectFile;
    if (tooOld || projectChanged) {
      this.logFn(
        `RONDA K5: guardarrail pendiente descartado (${tooOld ? 'caduco por turnos' : 'el proyecto abierto cambio'})`,
      );
      this.pending = null;
      this.persist();
    }
  }
}
