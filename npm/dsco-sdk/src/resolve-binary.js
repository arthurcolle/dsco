// resolve-binary.js — locate a dsco binary: explicit env, vendored sibling, PATH.
'use strict';
import { existsSync } from 'node:fs';
import path from 'node:path';

function platformTriple() {
  const p = process.platform, a = process.arch;
  if (p === 'darwin') return `dsco-darwin-${a === 'arm64' ? 'arm64' : 'x64'}`;
  if (p === 'linux') return `dsco-linux-${a === 'x64' ? 'x64' : a}`;
  throw new Error(`unsupported platform ${p}/${a}`);
}

export const platformTripleHint = platformTriple;

/**
 * Resolution order:
 *  1. DSCO_BIN env (absolute path)
 *  2. vendored binary from the @distributed.systems/dsco install package
 *     (node_modules/@distributed.systems/dsco/vendor/<triple>/dsco)
 *  3. "dsco" on PATH
 * @returns {string} absolute or PATH-resolvable binary path
 */
export function resolveBinary() {
  if (process.env.DSCO_BIN) {
    if (!existsSync(process.env.DSCO_BIN)) {
      throw new Error(`DSCO_BIN=${process.env.DSCO_BIN} does not exist`);
    }
    return process.env.DSCO_BIN;
  }
  // walk up from this module looking for the sibling npm package's vendor dir
  for (let p = import.meta.dirname; ; p = path.dirname(p)) {
    const cand = path.join(p, '@distributed.systems', 'dsco', 'vendor');
    if (existsSync(cand)) {
      const inner = path.join(cand, platformTriple(), 'dsco');
      if (existsSync(inner)) return inner;
      break;
    }
    if (p === '/' || p.endsWith('node_modules')) break;
  }
  return 'dsco'; // rely on PATH
}
