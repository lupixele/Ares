import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const webDir = path.resolve(__dirname, '../../src_assets/common/assets/web');

test('apps.html UI contract and API preservation', () => {
  const content = fs.readFileSync(path.join(webDir, 'apps.html'), 'utf8');

  // Ares navigation & body shell
  assert.ok(content.includes('<AresSidebar></AresSidebar>'), 'Must include AresSidebar component');
  assert.ok(content.includes('ares-page-body container'), 'Must use ares-page-body layout');

  // API Endpoints
  const requiredEndpoints = [
    './api/apps',
    './api/apps/delete',
    './api/apps/reorder',
    './api/apps/launch',
    './api/apps/close',
    './api/covers/upload',
    './api/config'
  ];
  for (const endpoint of requiredEndpoints) {
    assert.ok(content.includes(endpoint), `Must preserve endpoint: ${endpoint}`);
  }

  // CRUD & Feature handlers
  const requiredMethods = [
    'loadApps',
    'newApp',
    'editApp',
    'save',
    'showDeleteForm',
    'launchApp',
    'closeApp',
    'exportLauncherFile',
    'saveOrder',
    'alphabetizeApps',
    'showCoverFinder',
    'closeCoverFinder',
    'useCover',
    'onDragStart',
    'onDrop'
  ];
  for (const method of requiredMethods) {
    assert.ok(content.includes(method), `Must preserve method: ${method}`);
  }

  // Reactive state fields
  const requiredState = [
    'apps',
    'showEditForm',
    'actionDisabled',
    'editForm',
    'coverSearching',
    'coverCandidates',
    'currentApp',
    'hostUUID',
    'hostName',
    'listReordered'
  ];
  for (const state of requiredState) {
    assert.ok(content.includes(state), `Must include state: ${state}`);
  }

  // Artemis deep link & launcher format
  assert.ok(content.includes('art://launch?host_uuid='), 'Must support Artemis launch protocol link');
  assert.ok(content.includes('# Artemis app entry'), 'Must preserve Artemis .art launcher format');

  // Modern UI cards and tokens
  assert.ok(content.includes('ares-app-card'), 'Must render modernized ares-app-card elements');
  assert.ok(content.includes('ares-pulse-dot'), 'Must render animated running indicator');
  assert.ok(content.includes('config-save-bar'), 'Must include sticky save bar');
});

test('pin.html UI contract and pairing API preservation', () => {
  const content = fs.readFileSync(path.join(webDir, 'pin.html'), 'utf8');

  // Ares navigation & layout
  assert.ok(content.includes('<AresSidebar></AresSidebar>'), 'Must include AresSidebar component');
  assert.ok(content.includes('ares-page-body container'), 'Must use ares-page-body layout');

  // Pairing & Client API Endpoints
  const requiredEndpoints = [
    './api/pin',
    './api/pin/otp',
    './api/clients/list',
    './api/clients/update',
    './api/clients/unpair',
    './api/clients/unpair-all',
    './api/clients/disconnect'
  ];
  for (const endpoint of requiredEndpoints) {
    assert.ok(content.includes(endpoint), `Must preserve endpoint: ${endpoint}`);
  }

  // Pairing methods & handlers
  const requiredMethods = [
    'switchTab',
    'registerDevice',
    'requestOTP',
    'editHost',
    'saveHost',
    'refreshClients',
    'editClient',
    'cancelEdit',
    'saveClient',
    'disconnectClient',
    'unpairSingle',
    'unpairAll',
    'permToStr',
    'checkPermission',
    'togglePermission',
    'validateModeOverride'
  ];
  for (const method of requiredMethods) {
    assert.ok(content.includes(method), `Must preserve method: ${method}`);
  }

  // Permission bitmask definitions
  assert.ok(content.includes('permissionMapping = {'), 'Must preserve permissionMapping object');
  assert.ok(content.includes('input_controller'), 'Must contain input_controller permission');
  assert.ok(content.includes('server_cmd'), 'Must contain server_cmd permission');
  assert.ok(content.includes('launch'), 'Must contain launch permission');

  // Mode override validation regex
  assert.ok(content.includes('^\\d+x\\d+x\\d+'), 'Must validate display mode override format');

  // Modern UI card & item classes
  assert.ok(content.includes('ares-card'), 'Must render ares-card styling');
  assert.ok(content.includes('ares-client-item'), 'Must render ares-client-item styling');
});

test('config.html save body sanitization and dirty baseline behavior', () => {
  const content = fs.readFileSync(path.join(webDir, 'config.html'), 'utf8');

  // Stripping runtime keys before config save
  assert.ok(content.includes('delete r.platform;'), 'Must strip platform from config');
  assert.ok(content.includes('delete r.status;'), 'Must strip status from config');
  assert.ok(content.includes('delete r.version;'), 'Must strip version from config');

  // Command arrays sanitization during save
  assert.ok(
    content.includes('config.global_prep_cmd = this.global_prep_cmd.filter('),
    'Must filter empty global prep commands before save'
  );
  assert.ok(
    content.includes('config.global_state_cmd = this.global_state_cmd.filter('),
    'Must filter empty global state commands before save'
  );
  assert.ok(
    content.includes('config.server_cmd = this.server_cmd.filter('),
    'Must filter empty server commands before save'
  );

  // Fallback mode validation
  assert.ok(content.includes('fallbackDisplayModeCache = this.config.fallback_mode'), 'Must validate fallback mode cache');

  // Snapshot tracking
  assert.ok(content.includes('_takeSnapshot()'), 'Must maintain snapshot tracking');
  assert.ok(content.includes('isDirty()'), 'Must maintain isDirty computed property');
});

test('AresSidebar.vue focus-trap and dropdown safety', () => {
  const content = fs.readFileSync(path.join(webDir, 'AresSidebar.vue'), 'utf8');

  // Scoped dropdown check
  assert.ok(content.includes('isSidebarDropdownOpen()'), 'Must scope dropdown checks to sidebar element');
  assert.ok(
    content.includes('getVisibleFocusable'),
    'Must query only visible focusable elements for Tab trap'
  );

  // Tab trap must NOT be disabled globally when any dropdown is open
  assert.ok(
    !content.includes("if (e.key === 'Tab' && mobileOpen.value && sidebarEl.value && !isBootstrapDropdownOpen())"),
    'Must not globally disable Tab on dropdown open'
  );
});
