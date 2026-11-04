// ASSISTANT/src/trimEffects.ts
//
// FICHERO GENERADO -- NO EDITAR A MANO.
//   Fuente:     tools/shinytrims/trim_effects.json
//   Regenerar:  py tools/shinytrims/gen_trim_effects.py
//
// Espejo exacto de GmProject/scripts/mi3_trim_fx_list/mi3_trim_fx_list.gml.
// Lo consume el z.enum de tools.ts: un nombre de efecto que el motor no
// conoce tiene que morir en el schema, no dentro del engine (donde
// mi3_trim_fx_id lo leeria como 0 = 'sin efecto').

export const TRIM_EFFECTS = [
  'glowing',
  'periodic',
  'shimmer',
  'aurora',
  'bodysweep',
  'breath',
  'charge',
  'chrome',
  'kaleido',
  'lava_lamp',
  'molten',
  'nebula',
  'nocturne',
  'oilslick',
  'opal',
  'plasma',
  'rainbow_flow',
  'silk',
  'spark',
  'tide',
] as const;

/** Los 20 efectos + 'none' (apagar), que es lo que aceptan
 *  armor.set_trim y object.add_armor. */
export const TRIM_EFFECTS_WITH_NONE = ['none', ...TRIM_EFFECTS] as const;
