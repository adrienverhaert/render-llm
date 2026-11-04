// ASSISTANT/src/failure.ts
//
// RONDA UI (F8/F9): clasificador de turnos fallidos. Modulo APARTE a proposito:
// el bucle de index.ts no se puede testear sin red, pero esto si -- y el gate
// G1.a se apoya justo en eso.
//
// El texto del error viene en INGLES del servidor y cambia sin avisar; la
// senal ESTRUCTURADA (SDKAssistantMessageError / TerminalReason) es la buena
// cuando el SDK la da, asi que tiene prioridad.

export type TurnFailure = 'auth' | 'context' | 'bad_resume' | 'other';

const AUTH_RE = /failed to authenticate|oauth|authentication_failed|invalid[_ ]api[_ ]key|unauthorized|401/i;
const CONTEXT_RE = /prompt is too long|context (length|window) exceeded|too many tokens/i;
// FIX max-turns: ultimo recurso, por si algun dia el tope llega como excepcion
// sin senal estructurada. El texto ("Reached maximum number of turns (24)") es
// ingles del servidor: se mira DESPUES de terminal_reason / subtype, nunca antes.
const MAX_TURNS_RE = /maximum number of turns|max_turns|error_max_turns/i;

/**
 * FIX max-turns: ¿este tramo termino por AGOTAR el tope de turnos, y no por un
 * fallo de verdad? No es un hermano de TurnFailure a proposito -- no es un
 * fallo: el trabajo va bien, simplemente no cabia en un tramo, y quien llama
 * continua la MISMA sesion en vez de dar el turno por perdido.
 *
 * Senal estructurada primero, igual que classifyFailure: `terminal_reason`
 * 'max_turns' y `subtype` 'error_max_turns' del SDKResultError (sdk.d.ts).
 */
export function isMaxTurns(message: string, terminalReason?: string, subtype?: string): boolean {
  if (terminalReason === 'max_turns' || subtype === 'error_max_turns') return true;
  // Si el SDK dio OTRA razon estructurada, esa manda: no se adivina por texto.
  if (terminalReason !== undefined || subtype !== undefined) return false;
  return MAX_TURNS_RE.test(message);
}

/**
 * Clasifica un turno fallido. `structured` viene de terminal_reason /
 * SDKAssistantMessage.error cuando el SDK los dio; tiene PRIORIDAD sobre el
 * texto.
 */
export function classifyFailure(message: string, structured?: string): TurnFailure {
  if (structured === 'authentication_failed' || structured === 'oauth_org_not_allowed') return 'auth';
  if (structured === 'prompt_too_long') return 'context';
  if (AUTH_RE.test(message)) return 'auth';
  if (CONTEXT_RE.test(message)) return 'context';
  if (/resum|session/i.test(message)) return 'bad_resume';
  return 'other';
}
