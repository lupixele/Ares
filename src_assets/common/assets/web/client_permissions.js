/**
 * Exact client permission bit definitions from src/crypto.h.
 */
export const PERM_BITS = Object.freeze({
  // Input permissions (0x00001F00)
  input_controller: 1 << 8,  // 256
  input_touch: 1 << 9,       // 512
  input_pen: 1 << 10,        // 1024
  input_mouse: 1 << 11,      // 2048
  input_kbd: 1 << 12,        // 4096

  // Action permissions (0x07000000)
  list: 1 << 24,             // 16777216
  view: 1 << 25,             // 33554432
  launch: 1 << 26,           // 67108864
})

/**
 * Total editable permissions bitmask (0x07001F00 = 117448448).
 * Operations (clipboard, upload, server commands) are unimplemented in Sunshine
 * and must not be presented, but their existing bits are preserved silently.
 */
export const EDITABLE_PERM_MASK =
  PERM_BITS.input_controller |
  PERM_BITS.input_touch |
  PERM_BITS.input_pen |
  PERM_BITS.input_mouse |
  PERM_BITS.input_kbd |
  PERM_BITS.list |
  PERM_BITS.view |
  PERM_BITS.launch

/**
 * Convert a 32-bit permission bitmask into editable boolean flags.
 *
 * @param {number} perm - 32-bit integer permission mask.
 * @returns {object} Object with boolean flags for each editable permission.
 */
export function permToFlags(perm) {
  const p = Number(perm) >>> 0
  return {
    controller: (p & PERM_BITS.input_controller) !== 0,
    touch: (p & PERM_BITS.input_touch) !== 0,
    pen: (p & PERM_BITS.input_pen) !== 0,
    mouse: (p & PERM_BITS.input_mouse) !== 0,
    keyboard: (p & PERM_BITS.input_kbd) !== 0,
    list: (p & PERM_BITS.list) !== 0,
    view: (p & PERM_BITS.view) !== 0,
    launch: (p & PERM_BITS.launch) !== 0,
  }
}

/**
 * Convert editable boolean flags into an editable 32-bit bitmask.
 *
 * @param {object} flags - Object with boolean flags for each editable permission.
 * @returns {number} Unsigned 32-bit integer mask.
 */
export function flagsToMask(flags) {
  if (!flags) return 0
  let mask = 0
  if (flags.controller) mask |= PERM_BITS.input_controller
  if (flags.touch) mask |= PERM_BITS.input_touch
  if (flags.pen) mask |= PERM_BITS.input_pen
  if (flags.mouse) mask |= PERM_BITS.input_mouse
  if (flags.keyboard) mask |= PERM_BITS.input_kbd
  if (flags.list) mask |= PERM_BITS.list
  if (flags.view) mask |= PERM_BITS.view
  if (flags.launch) mask |= PERM_BITS.launch
  return mask >>> 0
}

/**
 * Compute the outgoing permission bitmask for POST /api/clients/update,
 * preserving any unassigned or non-editable bits (e.g. operations or reserved bits)
 * from originalPerm, while applying user modifications to editable fields.
 *
 * @param {number} originalPerm - Original permission integer from server.
 * @param {object} flags - Current editable boolean flags.
 * @returns {number} Outgoing 32-bit unsigned integer permission mask.
 */
export function computeOutgoingPerm(originalPerm, flags) {
  const original = Number(originalPerm) >>> 0
  const preservedNonEditable = (original & ~EDITABLE_PERM_MASK) >>> 0
  const editableBits = flagsToMask(flags)
  return (preservedNonEditable | editableBits) >>> 0
}

/**
 * Check if a client's draft represents a permission reduction or disable
 * compared to its acknowledged server baseline.
 *
 * @param {object} baseline - Baseline { enabled: boolean, flags: object }.
 * @param {object} draft - Draft { enabled: boolean, flags: object }.
 * @returns {boolean} True if client is being disabled or any view/input permission revoked.
 */
export function isReductionOrDisable(baseline, draft) {
  if (!baseline || !draft) return false
  if (baseline.enabled && !draft.enabled) return true
  if (baseline.flags.view && !draft.flags.view) return true
  if (baseline.flags.launch && !draft.flags.launch) return true
  const inputKeys = ['controller', 'touch', 'pen', 'mouse', 'keyboard']
  for (const key of inputKeys) {
    if (baseline.flags[key] && !draft.flags[key]) return true
  }
  return false
}
