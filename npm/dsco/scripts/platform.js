'use strict';

function platformTriple() {
  const platform = process.platform;
  const arch = process.arch;

  if (platform === 'darwin' && arch === 'arm64') return 'darwin-arm64';
  if (platform === 'darwin' && arch === 'x64') return 'darwin-x64';
  if (platform === 'linux' && arch === 'x64') return 'linux-x64';

  throw new Error(
    `Unsupported platform: ${platform}-${arch}. ` +
    `Supported: darwin-arm64, darwin-x64, linux-x64. ` +
    `Build from source or use Homebrew if available.`
  );
}

module.exports = { platformTriple };
