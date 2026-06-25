#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const https = require('https');
const { spawnSync } = require('child_process');
const { platformTriple } = require('./platform');

const pkg = require('../package.json');
const root = path.resolve(__dirname, '..');
const vendor = path.join(root, 'vendor');
const version = pkg.version;
const triple = platformTriple();

const binaryHost = process.env.DSCO_BINARY_HOST || 'https://github.com/arthurcolle/dsco/releases/download';
const url = `${binaryHost}/v${version}/dsco-${triple}.tar.gz`;
const archive = path.join(vendor, `dsco-${triple}.tar.gz`);

function log(msg) {
  console.log(`[dsco install] ${msg}`);
}

function fail(msg) {
  console.error(`[dsco install] ERROR: ${msg}`);
  process.exit(1);
}

function ensureDir(p) {
  fs.mkdirSync(p, { recursive: true });
}

function chmodIfExists(file) {
  if (fs.existsSync(file)) fs.chmodSync(file, 0o755);
}

function download(from, to) {
  return new Promise((resolve, reject) => {
    const file = fs.createWriteStream(to);
    const request = https.get(from, (res) => {
      if (res.statusCode === 301 || res.statusCode === 302 || res.statusCode === 307 || res.statusCode === 308) {
        file.close();
        fs.unlinkSync(to);
        download(res.headers.location, to).then(resolve, reject);
        return;
      }

      if (res.statusCode !== 200) {
        file.close();
        fs.unlinkSync(to);
        reject(new Error(`HTTP ${res.statusCode} from ${from}`));
        return;
      }

      res.pipe(file);
      file.on('finish', () => {
        file.close(resolve);
      });
    });

    request.on('error', (err) => {
      file.close();
      try { fs.unlinkSync(to); } catch (_) {}
      reject(err);
    });
  });
}

async function main() {
  ensureDir(vendor);

  if (process.env.DSCO_SKIP_DOWNLOAD === '1') {
    log('DSCO_SKIP_DOWNLOAD=1 set; skipping binary download');
    return;
  }

  const dscoPath = path.join(vendor, 'dsco');
  if (fs.existsSync(dscoPath)) {
    log(`binary already present at ${dscoPath}`);
    return;
  }

  log(`platform: ${triple}`);
  log(`downloading: ${url}`);

  try {
    await download(url, archive);
  } catch (err) {
    fail(
      `could not download DSCO binary: ${err.message}\n` +
      `Expected release asset: ${url}\n\n` +
      `If this is a new release, ensure release.yml uploaded dsco-${triple}.tar.gz.\n` +
      `Alternative: brew install arthurcolle/dsco/dsco`
    );
  }

  log('extracting archive');
  const tar = spawnSync('tar', ['-xzf', archive, '-C', vendor], { stdio: 'inherit' });
  if (tar.status !== 0) fail('tar extraction failed');

  chmodIfExists(path.join(vendor, 'dsco'));
  chmodIfExists(path.join(vendor, 'dsc'));
  chmodIfExists(path.join(vendor, 'dsco-lite'));

  try { fs.unlinkSync(archive); } catch (_) {}

  const smoke = spawnSync(path.join(vendor, 'dsco'), ['--version'], { encoding: 'utf8' });
  if (smoke.status !== 0) {
    fail(
      `downloaded dsco but smoke test failed.\n` +
      `stderr:\n${smoke.stderr || ''}\n` +
      `stdout:\n${smoke.stdout || ''}`
    );
  }

  log(`installed ${smoke.stdout.trim()}`);
}

main().catch((err) => fail(err && err.stack ? err.stack : String(err)));
