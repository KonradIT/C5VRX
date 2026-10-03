import { ESPLoader, Transport } from './esptool.js';
import { SerialTerminal } from './serial-terminal.js';

// DOM Elements
const btnConnect = document.getElementById('btnConnect');
const btnConnectText = document.getElementById('btnConnectText');
const btnFlash = document.getElementById('btnFlash');
const unsupportedWarning = document.getElementById('unsupportedWarning');

const tabGithub = document.getElementById('tabGithub');
const tabAlpha = document.getElementById('tabAlpha');
const paneAlpha = document.getElementById('paneAlpha');
const selectAlphaBuild = document.getElementById('selectAlphaBuild');
const selectAlphaPackageType = document.getElementById('selectAlphaPackageType');
const btnRefreshAlphaBuilds = document.getElementById('btnRefreshAlphaBuilds');
const alphaBuildDetails = document.getElementById('alphaBuildDetails');

const tabPr = document.getElementById('tabPr');
const tabLocal = document.getElementById('tabLocal');
const paneGithub = document.getElementById('paneGithub');
const panePr = document.getElementById('panePr');
const paneLocal = document.getElementById('paneLocal');

const selectRelease = document.getElementById('selectRelease');
const btnRefreshReleases = document.getElementById('btnRefreshReleases');
const selectPackageType = document.getElementById('selectPackageType');
const releaseDetails = document.getElementById('releaseDetails');

const selectPrBuild = document.getElementById('selectPrBuild');
const btnRefreshPrBuilds = document.getElementById('btnRefreshPrBuilds');
const selectPrPackageType = document.getElementById('selectPrPackageType');
const prBuildDetails = document.getElementById('prBuildDetails');
const prBuildWarning = document.getElementById('prBuildWarning');
const prBuildNumber = document.getElementById('prBuildNumber');
const infoPrBuildName = document.getElementById('infoPrBuildName');
const infoPrBuildDate = document.getElementById('infoPrBuildDate');
const infoPrBuildTag = document.getElementById('infoPrBuildTag');
const infoPrBuildAssets = document.getElementById('infoPrBuildAssets');

const infoReleaseName = document.getElementById('infoReleaseName');
const infoReleaseDate = document.getElementById('infoReleaseDate');
const infoReleaseTag = document.getElementById('infoReleaseTag');
const infoReleaseAssets = document.getElementById('infoReleaseAssets');

const dropzone = document.getElementById('dropzone');
const inputLocalFile = document.getElementById('inputLocalFile');
const localFileDetails = document.getElementById('localFileDetails');
const localFileName = document.getElementById('localFileName');
const localFileSize = document.getElementById('localFileSize');
const inputFlashOffset = document.getElementById('inputFlashOffset');

const selectBaud = document.getElementById('selectBaud');
const chkEraseAll = document.getElementById('chkEraseAll');

const statStatus = document.getElementById('statStatus');

const progressContainer = document.getElementById('progressContainer');
const progressBar = document.getElementById('progressBar');
const progressStatusText = document.getElementById('progressStatusText');
const progressPercentText = document.getElementById('progressPercentText');

const consoleOutput = document.getElementById('consoleOutput');
const btnClearConsole = document.getElementById('btnClearConsole');

// App State
let activeSource = 'github'; // 'github' | 'pr' | 'alpha' | 'local'
let port = null;
let transport = null;
let esploader = null;
let isConnected = false;
let isConnecting = false;
let isFlashing = false;
let githubReleases = [];
let githubPrBuilds = [];
let githubAlphaBuilds = [];
let localFileBinary = null;
let localFileNameStr = '';

// Known fallback releases if GitHub API rate-limits
const FALLBACK_RELEASES = [
  {
    tag_name: 'v3.0.0-rc1',
    name: 'C5VRX-3 v3.0.0-rc1: Seamless 16K Phase5 Production Receiver',
    published_at: '2026-09-18T17:01:49Z',
    prerelease: true,
    assets: [
      { name: 'c5vrx3.bin', size: 1082064, browser_download_url: 'https://github.com/Twotoz/C5VRX/releases/download/v3.0.0-rc1/c5vrx3.bin' },
      { name: 'bootloader.bin', size: 23232, browser_download_url: 'https://github.com/Twotoz/C5VRX/releases/download/v3.0.0-rc1/bootloader.bin' },
      { name: 'partition-table.bin', size: 3072, browser_download_url: 'https://github.com/Twotoz/C5VRX/releases/download/v3.0.0-rc1/partition-table.bin' },
    ]
  }
];

const VERSION_TAG_PATTERN = /^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;
const PR_BUILD_TAG_PATTERN = /^pr-(\d+)$/;

function getPrBuildNumber(rel) {
  const match = PR_BUILD_TAG_PATTERN.exec(rel?.tag_name || '');
  return match ? parseInt(match[1], 10) : null;
}

// Terminal output helper
function log(msg, type = 'info') {
  if (type === 'error') document.getElementById('flashLog').open = true;
  const time = new Date().toLocaleTimeString();
  const prefix = `[${time}] `;
  consoleOutput.textContent += prefix + msg + '\n';
  consoleOutput.scrollTop = consoleOutput.scrollHeight;
}

const terminal = {
  clean() {
    consoleOutput.textContent = '';
  },
  writeLine(data) {
    consoleOutput.textContent += data + '\n';
    consoleOutput.scrollTop = consoleOutput.scrollHeight;
  },
  write(data) {
    consoleOutput.textContent += data;
    consoleOutput.scrollTop = consoleOutput.scrollHeight;
  }
};

// Check Web Serial support
function checkSerialSupport() {
  if (!('serial' in navigator)) {
    unsupportedWarning.style.display = 'block';
    btnConnect.disabled = true;
    log('ERROR: Web Serial API not supported in this browser. Please use Google Chrome, MS Edge, or Brave.', 'error');
    return false;
  }
  return true;
}

// Tab Switching
function activateSource(source) {
  if (isFlashing) return;
  activeSource = source;
  for (const [button, name] of [[tabGithub, 'github'], [tabPr, 'pr'], [tabAlpha, 'alpha'], [tabLocal, 'local']]) {
    button.setAttribute('aria-pressed', String(source === name));
  }

  tabGithub.classList.toggle('active', source === 'github');
  tabPr.classList.toggle('active', source === 'pr');
  tabAlpha.classList.toggle('active', source === 'alpha');
  paneAlpha.classList.toggle('active', source === 'alpha');
  tabLocal.classList.toggle('active', source === 'local');

  paneGithub.classList.toggle('active', source === 'github');
  panePr.classList.toggle('active', source === 'pr');
  paneLocal.classList.toggle('active', source === 'local');

  updateFlashButtonState();
}

tabGithub.addEventListener('click', () => activateSource('github'));
tabAlpha.addEventListener('click', () => activateSource('alpha'));
btnRefreshAlphaBuilds.addEventListener('click', () => fetchReleases());
selectAlphaBuild.addEventListener('change', () => onAlphaBuildSelected());

tabPr.addEventListener('click', () => activateSource('pr'));
tabLocal.addEventListener('click', () => activateSource('local'));

// Drag & drop file handling
dropzone.addEventListener('click', (event) => {
  // The file input click bubbles too; never open the picker recursively.
  if (!isFlashing && event.target !== inputLocalFile) inputLocalFile.click();
});
dropzone.addEventListener('dragover', (e) => {
  e.preventDefault();
  dropzone.classList.add('dragover');
});
dropzone.addEventListener('dragleave', () => dropzone.classList.remove('dragover'));
dropzone.addEventListener('drop', (e) => {
  if (isFlashing) { e.preventDefault(); return; }
  e.preventDefault();
  dropzone.classList.remove('dragover');
  if (e.dataTransfer.files.length > 0) {
    handleLocalFile(e.dataTransfer.files[0]);
  }
});
inputLocalFile.addEventListener('change', (e) => {
  if (e.target.files.length > 0) {
    handleLocalFile(e.target.files[0]);
  }
});

function handleLocalFile(file) {
  if (!file.name.endsWith('.bin')) {
    alert('Please select a compiled firmware binary (.bin file)');
    return;
  }
  localFileNameStr = file.name;
  localFileName.textContent = file.name;
  localFileSize.textContent = `${(file.size / 1024).toFixed(1)} KB (${file.size.toLocaleString()} bytes)`;

  // Suggest default offset based on name
  if (file.name.toLowerCase().includes('merged')) {
    inputFlashOffset.value = '0x0';
  } else if (file.name.toLowerCase().includes('bootloader')) {
    inputFlashOffset.value = '0x2000';
  } else if (file.name.toLowerCase().includes('partition')) {
    inputFlashOffset.value = '0x8000';
  } else {
    inputFlashOffset.value = '0x10000';
  }

  const reader = new FileReader();
  reader.onload = (event) => {
    localFileBinary = event.target.result;
    localFileDetails.style.display = 'block';
    log(`Loaded local file: ${file.name} (${file.size} bytes)`);
    updateFlashButtonState();
  };
  reader.readAsArrayBuffer(file);
}

// Fetch the firmware index mirrored into the GitHub Pages artifact.
// Production flashing must stay same-origin: browsers cannot reliably fetch
// GitHub Release storage redirects due to CORS.
async function fetchReleases() {
  selectRelease.innerHTML = '<option value="">Fetching firmware index...</option>';
  selectPrBuild.innerHTML = '<option value="">Fetching PR builds...</option>';
  selectAlphaBuild.innerHTML = '<option value="">Fetching alpha builds...</option>';

  let data = null;
  try {
    const mirrorUrl = new URL('firmware/releases.json', document.baseURI);
    mirrorUrl.searchParams.set('t', Date.now().toString());
    const mirrorRes = await fetch(mirrorUrl.href, { cache: 'no-store' });
    if (!mirrorRes.ok) throw new Error(`Pages firmware index HTTP ${mirrorRes.status}`);
    data = await mirrorRes.json();
    if (!Array.isArray(data)) throw new Error('Pages firmware index is not an array');
    log(`Loaded same-origin firmware index from GitHub Pages (${data.length} build(s)).`);
  } catch (mirrorError) {
    // Metadata fallback only. Binary flashing still prefers same-origin assets;
    // this path mainly keeps local development usable for release browsing.
    log(`Warning: Pages firmware index unavailable (${mirrorError.message}); falling back to GitHub release metadata.`);
    try {
      const res = await fetch('https://api.github.com/repos/Twotoz/C5VRX/releases?per_page=100', {
        headers: { 'Accept': 'application/vnd.github.v3+json' },
        cache: 'no-store'
      });
      if (!res.ok) throw new Error(`GitHub API HTTP ${res.status}`);
      data = await res.json();
      if (!Array.isArray(data)) throw new Error('GitHub releases response is not an array');
    } catch (apiError) {
      log(`Warning: Failed to fetch release metadata (${apiError.message}). Using cached production release only.`);
      data = FALLBACK_RELEASES;
    }
  }

  let productionReleases = data
    .filter(rel => VERSION_TAG_PATTERN.test(rel.tag_name || ''))
    .sort((a, b) => new Date(b.published_at || 0) - new Date(a.published_at || 0));
  const prBuilds = data
    .filter(rel => rel.prerelease && PR_BUILD_TAG_PATTERN.test(rel.tag_name || ''))
    .sort((a, b) => new Date(b.published_at || 0) - new Date(a.published_at || 0));

  if (productionReleases.length === 0) {
    log('Warning: No versioned release in firmware index. Keeping the cached production release as a safe fallback.');
    productionReleases = FALLBACK_RELEASES;
  }

  githubReleases = productionReleases;
  githubPrBuilds = prBuilds;
  githubAlphaBuilds = data.filter(rel => rel.prerelease && /^c5vrx4-(?:alpha|pr-[0-9]+|v4\.[0-9]+\.[0-9]+-alpha\.[0-9]+)$/.test(rel.tag_name || ""))
    .sort((a, b) => (Number(/^c5vrx4-pr-/.test(a.tag_name)) - Number(/^c5vrx4-pr-/.test(b.tag_name))) || new Date(b.published_at || 0) - new Date(a.published_at || 0));
  if (githubAlphaBuilds.some(rel => rel.tag_name.startsWith("c5vrx4-v"))) {
    githubAlphaBuilds = githubAlphaBuilds.filter(rel => rel.tag_name !== "c5vrx4-alpha");
  }
  log(`Available: ${githubReleases.length} versioned release(s), ${githubPrBuilds.length} experimental PR build(s).`);

  populateReleaseDropdown();
  populatePrBuildDropdown();
  populateAlphaBuildDropdown();
  updateFlashButtonState();
}

function populateAlphaBuildDropdown() {
  selectAlphaBuild.replaceChildren();
  githubAlphaBuilds.forEach((rel, index) => {
    const opt = document.createElement('option');
    opt.value = index;
    opt.textContent = rel.tag_name.startsWith('c5vrx4-v')
      ? `C5VRX-4 ${rel.tag_name.slice('c5vrx4-'.length)}${index === 0 ? ' (Latest alpha)' : ''}`
      : rel.tag_name === 'c5vrx4-alpha'
      ? 'C5VRX-4 Alpha — latest main'
      : `C5VRX-4 Alpha — PR #${rel.tag_name.split('-').pop()} / unmerged`;
    selectAlphaBuild.appendChild(opt);
  });
  selectAlphaBuild.disabled = githubAlphaBuilds.length === 0;
  if (githubAlphaBuilds.length) {
    selectAlphaBuild.value = '0';
    onAlphaBuildSelected();
  } else {
    selectAlphaBuild.innerHTML = '<option value="">No alpha builds published yet</option>';
    alphaBuildDetails.style.display = 'none';
  }
}

function onAlphaBuildSelected() {
  const rel = githubAlphaBuilds[Number(selectAlphaBuild.value)];
  if (!rel) return;
  alphaBuildDetails.style.display = 'block';
  for (const [id, value] of [
    ['infoAlphaBuildName', rel.name || rel.tag_name],
    ['infoAlphaBuildDate', rel.published_at ? new Date(rel.published_at).toLocaleDateString() : 'N/A'],
    ['infoAlphaBuildTag', rel.tag_name],
    ['infoAlphaBuildAssets', (rel.assets || []).map(a => a.name).join(', ')]
  ]) document.getElementById(id).textContent = value;
  updateFlashButtonState();
}

function populateReleaseDropdown() {
  selectRelease.innerHTML = '';

  githubReleases.forEach((rel, index) => {
    const opt = document.createElement('option');
    opt.value = index;
    const tag = rel.tag_name || rel.name;
    const isLatest = index === 0;
    opt.textContent = rel.prerelease
      ? `${tag} [Pre-release]${isLatest ? ' (Latest available)' : ''}`
      : `${tag}${isLatest ? ' (Latest - Recommended)' : ''}`;
    selectRelease.appendChild(opt);
  });

  if (githubReleases.length > 0) {
    selectRelease.disabled = false;
    selectRelease.value = '0';
    onReleaseSelected(0);
  } else {
    selectRelease.disabled = true;
    selectRelease.innerHTML = '<option value="">No versioned releases available</option>';
    releaseDetails.style.display = 'none';
  }
}

function populatePrBuildDropdown() {
  selectPrBuild.innerHTML = '';

  githubPrBuilds.forEach((rel, index) => {
    const opt = document.createElement('option');
    const prNumber = getPrBuildNumber(rel);
    opt.value = index;
    opt.textContent = `PR #${prNumber} — EXPERIMENTAL / UNMERGED`;
    selectPrBuild.appendChild(opt);
  });

  if (githubPrBuilds.length > 0) {
    selectPrBuild.disabled = false;
    selectPrBuild.value = '0';
    onPrBuildSelected(0);
  } else {
    selectPrBuild.disabled = true;
    selectPrBuild.innerHTML = '<option value="">No active PR builds available</option>';
    prBuildDetails.style.display = 'none';
    prBuildNumber.textContent = '';
  }
}

selectRelease.addEventListener('change', () => {
  onReleaseSelected(parseInt(selectRelease.value, 10));
});

selectPrBuild.addEventListener('change', () => {
  onPrBuildSelected(parseInt(selectPrBuild.value, 10));
});

btnRefreshReleases.addEventListener('click', () => {
  log('Refreshing releases from GitHub...');
  fetchReleases();
});

btnRefreshPrBuilds.addEventListener('click', () => {
  log('Refreshing PR builds from GitHub...');
  fetchReleases();
});

function onReleaseSelected(index) {
  const rel = githubReleases[index];
  if (!rel) return;

  releaseDetails.style.display = 'block';
  infoReleaseName.textContent = rel.name || rel.tag_name;
  infoReleaseDate.textContent = rel.published_at ? new Date(rel.published_at).toLocaleDateString() : 'N/A';
  infoReleaseTag.textContent = rel.tag_name;
  infoReleaseAssets.textContent = (rel.assets || []).map(a => a.name).join(', ') || 'No binary assets attached';

  updateFlashButtonState();
}

function onPrBuildSelected(index) {
  const rel = githubPrBuilds[index];
  if (!rel) return;

  const prNumber = getPrBuildNumber(rel);
  prBuildDetails.style.display = 'block';
  prBuildNumber.textContent = prNumber !== null ? `#${prNumber}` : '';
  infoPrBuildName.textContent = rel.name || rel.tag_name;
  infoPrBuildDate.textContent = rel.published_at ? new Date(rel.published_at).toLocaleDateString() : 'N/A';
  infoPrBuildTag.textContent = rel.tag_name;
  infoPrBuildAssets.textContent = (rel.assets || []).map(a => a.name).join(', ') || 'No binary assets attached';

  updateFlashButtonState();
}

function updateFlashButtonState() {
  if (!isConnected || isFlashing) {
    btnFlash.disabled = true;
    return;
  }

  if (activeSource === 'github') {
    const idx = parseInt(selectRelease.value, 10);
    btnFlash.disabled = !Number.isInteger(idx) || !githubReleases[idx];
  } else if (activeSource === 'alpha') {
    const idx = parseInt(selectAlphaBuild.value, 10);
    btnFlash.disabled = !Number.isInteger(idx) || !githubAlphaBuilds[idx];
  } else if (activeSource === 'pr') {
    const idx = parseInt(selectPrBuild.value, 10);
    btnFlash.disabled = !Number.isInteger(idx) || !githubPrBuilds[idx];
  } else {
    btnFlash.disabled = (localFileBinary === null);
  }
}

function getSelectedRemoteBuild() {
  if (activeSource === 'github') {
    return {
      release: githubReleases[parseInt(selectRelease.value, 10)],
      packageType: selectPackageType.value
    };
  }
  if (activeSource === 'pr') {
    return {
      release: githubPrBuilds[parseInt(selectPrBuild.value, 10)],
      packageType: selectPrPackageType.value
    };
  }
  if (activeSource === 'alpha') {
    return { release: githubAlphaBuilds[parseInt(selectAlphaBuild.value, 10)], packageType: selectAlphaPackageType.value };
  }
  return { release: null, packageType: null };
}

function findApplicationAsset(assets) {
  // GitHub returns release assets in upload order. Never use the first .bin:
  // bootloader.bin is commonly uploaded before the application image.
  return assets.find(asset => /^c5vrx(?:[34])?\.bin$/i.test(asset.name || '')) ||
    assets.find(asset => {
      const name = (asset.name || '').toLowerCase();
      return name.endsWith('.bin') &&
        !name.includes('bootloader') &&
        !name.includes('partition') &&
        !name.includes('merged');
    });
}

// Connect / Disconnect Handler
btnConnect.addEventListener('click', async () => {
  if (isFlashing || isConnecting) return;
  if (isConnected) {
    await disconnectDevice();
  } else {
    await connectDevice();
  }
});

async function connectDevice() {
  if (isConnecting || isConnected || isFlashing || serialConsole.state !== 'disconnected' || !checkSerialSupport()) return;
  isConnecting = true;
  syncDeviceControls();

  try {
    log('Opening Web Serial port selector...');
    port = await navigator.serial.requestPort();
    log('Port selected. Initializing connection...');

    statStatus.textContent = 'Connecting...';
    btnConnect.disabled = true;

    transport = new Transport(port);
    const baudrate = parseInt(selectBaud.value, 10) || 460800;

    esploader = new ESPLoader({
      transport: transport,
      baudrate: baudrate,
      terminal: terminal,
      romBaudrate: 115200,
      debugLogging: false
    });

    log('Handshaking with ESP32-C5 ROM bootloader...');
    // Attempt connection with hardware reset
    await esploader.main("usb_reset");

    isConnected = true;

    btnConnectText.textContent = 'Disconnect';
    syncDeviceControls();

    const chipName = esploader.chip ? esploader.chip.CHIP_NAME : 'ESP32-C5';

    statStatus.textContent = `${chipName} connected · Ready to flash`;
    log(`Connected successfully to ${chipName}!`);
    updateFlashButtonState();

  } catch (err) {
    log(`Connection failed: ${err.message || err}`, 'error');
    alert(`Failed to connect: ${err.message || err}\n\nTroubleshooting Tip:\nIf port timed out, hold BOOT (B) on the XIAO board, tap RESET (R), and release BOOT to force ROM bootloader mode.`);
    await disconnectDevice();
  } finally {
    isConnecting = false;
    syncDeviceControls();
  }
}

async function disconnectDevice() {
  try {
    if (transport) {
      await transport.disconnect();
    }
  } catch (e) {
    // Ignore close errors
  }
  port = null;
  transport = null;
  esploader = null;
  isConnected = false;

  btnConnectText.textContent = 'Connect device';
  syncDeviceControls();

  statStatus.textContent = 'No device connected';

  syncDeviceControls();
  log('Device disconnected.');
  updateFlashButtonState();
}

// Flashing Handler
btnFlash.addEventListener('click', async () => {
  if (!isConnected || !esploader || isFlashing || serialConsole.state !== 'disconnected') return;

  if (activeSource === 'pr') {
    const selectedBuild = githubPrBuilds[parseInt(selectPrBuild.value, 10)];
    const prNumber = getPrBuildNumber(selectedBuild);
    const accepted = window.confirm(
      `WARNING: PR #${prNumber ?? '?'} is an experimental, unmerged test build.\n\n` +
      'It may be unstable, fail to boot, corrupt settings, or produce broken video. ' +
      'Only continue if you intentionally want to test this pull request.\n\nFlash it anyway?'
    );
    if (!accepted) {
      log(`Cancelled experimental PR #${prNumber ?? '?'} flash.`);
      return;
    }
  }

  if (activeSource === 'alpha' && !window.confirm('Flash C5VRX-4 Alpha? This experimental firmware may fail to boot or produce broken video. Use Full firmware when switching generations.')) return;

  isFlashing = true;
  syncDeviceControls();
  document.getElementById('flashLog').open = true;
  btnFlash.disabled = true;
  btnConnect.disabled = true;
  progressContainer.style.display = 'block';
  progressBar.style.width = '0%';
  progressPercentText.textContent = '0%';
  progressStatusText.textContent = 'Preparing firmware images...';

  try {
    const fileArray = [];

    if (activeSource !== 'local') {
      const selected = getSelectedRemoteBuild();
      const rel = selected.release;
      if (!rel) throw new Error(activeSource === 'pr' ? 'No PR build selected' : 'No release selected');
      const assets = rel.assets || [];
      const packageType = selected.packageType;

      log(`Fetching binaries for ${activeSource === 'pr' ? 'PR build' : 'release'} ${rel.tag_name} (${packageType})...`);

      if (packageType === 'merged') {
        const mergedAsset = assets.find(a => a.name.includes('merged'));
        if (mergedAsset) {
          log(`Downloading ${mergedAsset.name}...`);
          const buf = await fetchReleaseAsset(mergedAsset);
          fileArray.push({ data: new Uint8Array(buf), address: 0x0 });
        } else {
          // Standard 3-part layout
          const bootloader = assets.find(a => a.name.includes('bootloader'));
          const ptable = assets.find(a => a.name.includes('partition'));
          const app = findApplicationAsset(assets);
          const missing = [
            !bootloader && 'bootloader',
            !ptable && 'partition table',
            !app && 'application firmware',
          ].filter(Boolean);
          if (missing.length > 0) {
            throw new Error(`Incomplete full firmware package: missing ${missing.join(', ')}`);
          }

          log(`Downloading bootloader (${bootloader.name})...`);
          const bBuf = await fetchReleaseAsset(bootloader);
          fileArray.push({ data: new Uint8Array(bBuf), address: 0x2000 });
          log(`Downloading partition table (${ptable.name})...`);
          const pBuf = await fetchReleaseAsset(ptable);
          fileArray.push({ data: new Uint8Array(pBuf), address: 0x8000 });
          log(`Downloading app binary (${app.name})...`);
          const aBuf = await fetchReleaseAsset(app);
          fileArray.push({ data: new Uint8Array(aBuf), address: 0x10000 });
        }
      } else {
        // App only
        const app = findApplicationAsset(assets);
        if (!app) throw new Error('Could not find application firmware binary in release assets');
        log(`Downloading app binary (${app.name})...`);
        const aBuf = await fetchReleaseAsset(app);
        fileArray.push({ data: new Uint8Array(aBuf), address: 0x10000 });
      }
    } else {
      // Local File
      if (!localFileBinary) throw new Error('No local binary selected');
      let offset = parseInt(inputFlashOffset.value.trim(), 16);
      if (isNaN(offset)) offset = 0x0;
      fileArray.push({
        data: new Uint8Array(localFileBinary),
        address: offset
      });
    }

    if (fileArray.length === 0) throw new Error('No files to flash');

    const totalBytes = fileArray.reduce((acc, f) => acc + f.data.length, 0);
    log(`Starting flash operation: ${fileArray.length} file(s), ${totalBytes.toLocaleString()} total bytes.`);

    progressStatusText.textContent = 'Flashing...';

    const flashOptions = {
      fileArray: fileArray,
      flashSize: '8MB',
      flashMode: 'dio',
      flashFreq: '80m',
      eraseAll: chkEraseAll.checked,
      // ESP32-C5 native USB-Serial/JTAG can fail mid-write in the compressed path (status 201,0).
      compress: false,
      reportProgress: (fileIndex, written, total) => {
        const percent = Math.floor((written / total) * 100);
        progressBar.style.width = `${percent}%`;
        progressPercentText.textContent = `${percent}%`;
        progressStatusText.textContent = `Writing file ${fileIndex + 1}/${fileArray.length} (${(written / 1024).toFixed(0)}KB / ${(total / 1024).toFixed(0)}KB)`;
      }
    };

    await esploader.writeFlash(flashOptions);

    progressBar.style.width = '100%';
    progressPercentText.textContent = '100%';
    progressStatusText.textContent = 'Flash Complete! Resetting device...';
    log('Flash written and hash verified successfully!');

    log('Hard resetting device into new firmware...');
    try {
      await esploader.after('hard_reset');
    } catch (e) {
      // Hardware reset pulse might close port on USB-CDC
    }

    log('====================================================');
    log('FLASHING COMPLETED SUCCESSFULLY!');
    log('Your C5VRX receiver is now running the new firmware.');
    log('====================================================');
    alert('Flashing completed successfully! Device has been reset.');

  } catch (err) {
    log(`Flashing failed: ${err.message || err}`, 'error');
    alert(`Flashing error: ${err.message || err}`);
    progressStatusText.textContent = 'Flash Failed';
  } finally {
    isFlashing = false;
    syncDeviceControls();
    updateFlashButtonState();
  }
});

async function fetchReleaseAsset(asset) {
  if (!asset) throw new Error('Missing firmware asset metadata');

  if (asset.local_url) {
    const localUrl = new URL(asset.local_url, document.baseURI);
    log(`Downloading ${asset.name} from same-origin Pages firmware mirror...`);
    const localRes = await fetch(localUrl.href, { cache: 'no-store' });
    if (!localRes.ok) {
      throw new Error(`Pages firmware mirror HTTP ${localRes.status} for ${asset.name}`);
    }
    const buffer = await localRes.arrayBuffer();
    if (asset.size && buffer.byteLength !== asset.size) {
      throw new Error(
        `Firmware mirror size mismatch for ${asset.name}: expected ${asset.size}, got ${buffer.byteLength}`
      );
    }
    return buffer;
  }

  // Development fallback only. Production Pages manifests always attach a
  // local_url. GitHub's release storage redirect may still be blocked by CORS.
  if (asset.url) {
    try {
      log(`No Pages mirror URL for ${asset.name}; trying GitHub asset API...`);
      const apiRes = await fetch(asset.url, {
        method: 'GET',
        headers: {
          'Accept': 'application/octet-stream',
          'X-GitHub-Api-Version': '2022-11-28'
        },
        redirect: 'follow',
        cache: 'no-store'
      });
      if (!apiRes.ok) throw new Error(`GitHub asset API HTTP ${apiRes.status}`);
      return await apiRes.arrayBuffer();
    } catch (apiError) {
      log(`GitHub asset API failed (${apiError.message}); trying browser download URL...`);
    }
  }

  if (asset.browser_download_url) {
    try {
      const directRes = await fetch(asset.browser_download_url, {
        method: 'GET',
        redirect: 'follow',
        cache: 'no-store'
      });
      if (!directRes.ok) throw new Error(`GitHub download HTTP ${directRes.status}`);
      return await directRes.arrayBuffer();
    } catch (directError) {
      throw new Error(
        `This build is not mirrored on GitHub Pages and GitHub's cross-origin download was blocked. ` +
        `Refresh the flasher after the Pages sync completes. Details: ${directError.message}`
      );
    }
  }

  throw new Error(`No downloadable URL available for ${asset.name || 'firmware asset'}`);
}

btnClearConsole.addEventListener('click', () => {
  terminal.clean();
  log('Console cleared.');
});

// Initialization
document.addEventListener('DOMContentLoaded', () => {
  log('C5VRX Web Flasher initialized.');
  checkSerialSupport();
  syncDeviceControls();
  fetchReleases();
});

// Firmware and terminal own separate transports and never open concurrently.
const serialOutput = document.getElementById('output');
const commandInput = document.getElementById('commandInput');
const btnTerminalConnect = document.getElementById('btnTerminalConnect');
const terminalBaud = document.getElementById('terminalBaud');
const terminalStatus = document.getElementById('terminalStatus');
const shortcutButtons = [...document.querySelectorAll('[data-command]')];
const MAX_LOG_CHARS = 120000;
let commandBusy = false;

function appendSerial(text) {
  serialOutput.textContent = (serialOutput.textContent + text).slice(-MAX_LOG_CHARS);
  if (document.getElementById('autoScroll').checked) serialOutput.scrollTop = serialOutput.scrollHeight;
}

const serialConsole = new SerialTerminal({
  serial: navigator.serial,
  onData: appendSerial,
  onState: (state) => {
    const labels = { connected: 'Connected', connecting: 'Connecting…', disconnecting: 'Disconnecting…', disconnected: 'No device connected' };
    terminalStatus.textContent = labels[state];
    if (state === 'connecting') serialOutput.textContent = '';
    syncDeviceControls();
  },
  onError: (error) => {
    appendSerial(`\n${error.message || error}\n`);
  }
});

function syncDeviceControls() {
  const supported = 'serial' in navigator;
  for (const control of [tabGithub, tabPr, tabAlpha, tabLocal, selectAlphaBuild, selectAlphaPackageType, btnRefreshAlphaBuilds, selectRelease, selectPrBuild, selectPackageType, selectPrPackageType, selectBaud, chkEraseAll, inputLocalFile, inputFlashOffset, btnRefreshReleases, btnRefreshPrBuilds, document.getElementById('btnBrowseFile')]) {
    control.disabled = isFlashing;
  }
  const terminalConnected = serialConsole.state === 'connected';
  const terminalIdle = serialConsole.state === 'disconnected';
  const flashBusy = isConnected || isConnecting || isFlashing;
  btnConnect.disabled = !supported || isConnecting || isFlashing || !terminalIdle;
  btnConnect.title = terminalIdle ? '' : 'Disconnect the terminal before connecting the flasher';
  btnTerminalConnect.disabled = !supported || flashBusy || (!terminalIdle && !terminalConnected);
  btnTerminalConnect.textContent = terminalConnected ? 'Disconnect' : serialConsole.state === 'connecting' ? 'Connecting…' : 'Connect';
  btnTerminalConnect.title = flashBusy ? 'Disconnect the flasher before connecting the terminal' : '';
  terminalBaud.disabled = !terminalIdle;
  commandInput.disabled = !terminalConnected || commandBusy;
  document.getElementById('btnSendCommand').disabled = !terminalConnected || commandBusy;
  for (const button of shortcutButtons) button.disabled = !terminalConnected || commandBusy;
}

btnTerminalConnect.addEventListener('click', async () => {
  if (isConnected || isConnecting || isFlashing) return;
  if (serialConsole.state === 'connected') await serialConsole.disconnect();
  else if (serialConsole.state === 'disconnected' && checkSerialSupport()) await serialConsole.connect(Number(terminalBaud.value));
});

async function sendTerminalCommand(command) {
  if (commandBusy || serialConsole.state !== 'connected') return false;
  commandBusy = true;
  syncDeviceControls();
  try { return await serialConsole.send(command); }
  finally { commandBusy = false; syncDeviceControls(); }
}

for (const button of shortcutButtons) {
  button.addEventListener('click', async () => {
    if (button.dataset.confirm && !window.confirm(button.dataset.confirm)) return;
    await sendTerminalCommand(button.dataset.command);
  });
}

document.getElementById('commandForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const command = commandInput.value;
  if (!command) return;
  const ending = document.getElementById('ending').value;
  const suffix = ending === 'lf' ? '\n' : ending === 'crlf' ? '\r\n' : '';
  if (await sendTerminalCommand(command + suffix)) commandInput.value = '';
});
document.getElementById('clear').addEventListener('click', () => { serialOutput.textContent = ''; });

for (const [buttonId, terminalView] of [['firmwareTab', false], ['terminalTab', true]]) {
  document.getElementById(buttonId).addEventListener('click', () => {
    document.getElementById('firmwareView').hidden = terminalView;
    document.getElementById('terminalView').hidden = !terminalView;
    for (const [id, active] of [['firmwareTab', !terminalView], ['terminalTab', terminalView]]) {
      const button = document.getElementById(id);
      button.classList.toggle('active', active);
      button.setAttribute('aria-pressed', String(active));
    }
  });
}

navigator.serial?.addEventListener('disconnect', async (event) => {
  if (event.target === serialConsole.port) await serialConsole.disconnect();
  if (event.target === port && !isFlashing && !isConnecting) await disconnectDevice();
});


// Keep docs visible on desktop and collapsible on smaller screens.
const docsBreakpoint = window.matchMedia('(min-width:801px)');
const docsNav = document.getElementById('docsNav');
docsNav.open = docsBreakpoint.matches;
docsBreakpoint.addEventListener('change', (event) => {
  docsNav.open = event.matches;
});
