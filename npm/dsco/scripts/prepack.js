#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const pkg = require('../package.json');

if (!pkg.name.startsWith('@distributed.systems/')) {
  console.error('Package must be published under @distributed.systems scope');
  process.exit(1);
}

for (const file of ['README.md', 'LICENSE', 'bin/dsco.js', 'scripts/install.js']) {
  if (!fs.existsSync(path.join(root, file))) {
    console.error(`Missing required npm package file: ${file}`);
    process.exit(1);
  }
}
