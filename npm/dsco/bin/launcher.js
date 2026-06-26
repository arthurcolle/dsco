#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

function main(binaryName) {
  const bin = path.resolve(__dirname, '..', 'vendor', binaryName);

  if (!fs.existsSync(bin)) {
    console.error(`[dsco] Missing binary: ${bin}`);
    console.error('[dsco] Reinstall with: npm install -g @distributed.systems/dsco');
    console.error('[dsco] Or install with Homebrew: brew install arthurcolle/dsco/dsco');
    process.exit(1);
  }

  const child = spawn(bin, process.argv.slice(2), { stdio: 'inherit' });
  child.on('exit', (code, signal) => {
    if (signal) {
      process.kill(process.pid, signal);
      return;
    }
    process.exit(code == null ? 1 : code);
  });
}

module.exports = { main };
