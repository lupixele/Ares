import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const aresRoot = path.resolve(__dirname, '..', '..');

test('[Host Migration Contract] Pinned official Sunshine and Apollo references exist', () => {
  const gitignorePath = path.join(aresRoot, '.gitignore');
  assert.ok(fs.existsSync(gitignorePath), '.gitignore must exist');
  const gitignoreContent = fs.readFileSync(gitignorePath, 'utf8');
  assert.ok(gitignoreContent.includes('.migration-worktrees/'), '.gitignore must ignore .migration-worktrees/');

  const worktreeSunshinePath = path.join(aresRoot, '.migration-worktrees', 'sunshine');
  assert.ok(fs.existsSync(worktreeSunshinePath), 'Isolated Sunshine worktree must exist');
  assert.ok(fs.existsSync(path.join(worktreeSunshinePath, 'CMakeLists.txt')), 'Sunshine worktree must contain CMakeLists.txt');
});

test('[Host Migration Contract] Apollo SudoVDA and virtual display header boundary integrity', () => {
  const vdisplayHeader = path.join(aresRoot, 'src', 'platform', 'windows', 'virtual_display.h');
  assert.ok(fs.existsSync(vdisplayHeader), 'virtual_display.h must exist in Ares');
  const content = fs.readFileSync(vdisplayHeader, 'utf8');

  // Verify critical SudoVDA symbols and signatures required by Apollo VDD lifecycle
  assert.ok(content.includes('namespace VDISPLAY'), 'Must declare namespace VDISPLAY');
  assert.ok(content.includes('enum class DRIVER_STATUS'), 'Must declare DRIVER_STATUS');
  assert.ok(content.includes('createVirtualDisplay'), 'Must export createVirtualDisplay');
  assert.ok(content.includes('removeVirtualDisplay'), 'Must export removeVirtualDisplay');
  assert.ok(content.includes('changeDisplaySettings2'), 'Must export changeDisplaySettings2 for isolated mode');
  assert.ok(content.includes('startPingThread'), 'Must export startPingThread watchdog');
});

test('[Host Migration Contract] Modernized Ares UI rollback preservation', () => {
  const sidebarVue = path.join(aresRoot, 'src_assets', 'common', 'assets', 'web', 'AresSidebar.vue');
  const configHtml = path.join(aresRoot, 'src_assets', 'common', 'assets', 'web', 'config.html');
  const appsHtml = path.join(aresRoot, 'src_assets', 'common', 'assets', 'web', 'apps.html');
  const pinHtml = path.join(aresRoot, 'src_assets', 'common', 'assets', 'web', 'pin.html');

  assert.ok(fs.existsSync(sidebarVue), 'AresSidebar.vue must be preserved');
  assert.ok(fs.existsSync(configHtml), 'config.html must be preserved');
  assert.ok(fs.existsSync(appsHtml), 'apps.html must be preserved');
  assert.ok(fs.existsSync(pinHtml), 'pin.html must be preserved');
});
