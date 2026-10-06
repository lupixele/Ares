<template>
  <Navbar></Navbar>
  <div id="content" class="container ares-overview-container">

    <!-- Overview Header -->
    <div class="ares-overview__header my-4">
      <div class="d-flex align-items-center justify-content-between flex-wrap gap-2">
        <div>
          <h1 class="ares-overview__title">{{ $t('index.welcome') }}</h1>
          <p class="ares-overview__desc text-muted mb-0">{{ $t('index.description') }}</p>
        </div>
        <div class="ares-overview__badge-wrapper" v-if="!configLoading">
          <span class="badge bg-primary-subtle text-primary-emphasis border border-primary-subtle px-3 py-2">
            <GitBranch :size="15" class="icon me-1"></GitBranch>
            {{ $t('index.ares_dev_build') }}
          </span>
        </div>
      </div>
    </div>

    <!-- Overview Stat Cards -->
    <div class="row g-3 mb-4 ares-stat-row">
      <!-- Card 1: Ares Version & Build -->
      <div class="col-12 col-md-4">
        <div class="card h-100 ares-stat-card">
          <div class="card-body">
            <div class="ares-stat-card__label">
              <GitBranch :size="14" class="icon me-1"></GitBranch>
              {{ $t('index.stat_version') }}
            </div>
            <div class="ares-stat-card__value">
              <span v-if="configLoading" class="text-muted fs-5">
                <Loader2 :size="18" class="icon me-1 fa-spin"></Loader2>
                {{ $t('index.config_loading') }}
              </span>
              <span v-else-if="configError" class="text-danger fs-5">
                {{ $t('index.config_offline') }}
              </span>
              <span v-else>
                {{ version ? version.version : '—' }}
              </span>
            </div>
            <div class="ares-stat-card__sub">
              <span v-if="configLoading" class="text-muted">...</span>
              <span v-else-if="configError" class="badge bg-danger-subtle text-danger-emphasis">
                {{ $t('index.config_offline') }}
              </span>
              <span v-else-if="buildVersionIsDirty" class="badge bg-success-subtle text-success-emphasis">
                <Package :size="12" class="icon me-1"></Package>
                {{ $t('index.version_dirty') }}
              </span>
              <span v-else-if="installedVersionNotStable" class="badge bg-info-subtle text-info-emphasis">
                <Info :size="12" class="icon me-1"></Info>
                Pre-release
              </span>
              <span v-else class="badge bg-secondary-subtle text-secondary-emphasis">
                {{ $t('index.ares_dev_build') }}
              </span>
            </div>
          </div>
        </div>
      </div>

      <!-- Card 2: Host Environment & Driver Backend -->
      <div class="col-12 col-md-4">
        <div class="card h-100 ares-stat-card">
          <div class="card-body">
            <div class="ares-stat-card__label">
              <Cpu :size="14" class="icon me-1"></Cpu>
              {{ $t('index.stat_host_platform') }}
            </div>
            <div class="ares-stat-card__value">
              <span v-if="configLoading" class="text-muted fs-5">...</span>
              <span v-else-if="configError" class="text-muted fs-5">—</span>
              <span v-else>{{ platformDisplay }}</span>
            </div>
            <div class="ares-stat-card__sub">
              <span v-if="configLoading" class="text-muted">...</span>
              <span v-else-if="configError" class="text-muted">—</span>
              <span v-else-if="!controllerEnabled" class="badge bg-secondary-subtle text-secondary-emphasis">
                {{ $t('index.stat_controller_disabled') }}
              </span>
              <span v-else-if="gamepadDriverDisplay" class="badge bg-primary-subtle text-primary-emphasis">
                <Gamepad2 :size="12" class="icon me-1"></Gamepad2>
                {{ gamepadDriverDisplay }}
              </span>
              <span v-else class="badge bg-success-subtle text-success-emphasis">
                {{ $t('index.stat_driver_ready') }}
              </span>
            </div>
          </div>
        </div>
      </div>

      <!-- Card 3: Client Pairing Quicklink -->
      <div class="col-12 col-md-4">
        <div class="card h-100 ares-stat-card">
          <div class="card-body d-flex flex-column justify-content-between">
            <div>
              <div class="ares-stat-card__label">
                <Key :size="14" class="icon me-1"></Key>
                {{ $t('index.stat_pairing') }}
              </div>
              <div class="ares-stat-card__value">
                <span v-if="pairedClientsCount !== null">{{ pairedClientsCount }} {{ $t('index.paired_clients') }}</span>
                <span v-else>{{ $t('index.stat_pairing_action') }}</span>
              </div>
            </div>
            <div class="ares-stat-card__sub mt-2">
              <RouterLink to="/pin" class="btn btn-sm btn-outline-primary d-inline-flex align-items-center gap-1">
                <Key :size="12" class="icon"></Key>
                {{ $t('index.pair_with_pin') }}
              </RouterLink>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Needed Attention Section: Fatal Startup Errors Alert -->
    <div class="alert alert-danger my-4" v-if="fancyLogs.some(x => x.level === 'Fatal')">
      <div>
        <div class="d-flex align-items-center mb-3">
          <AlertCircle :size="32" class="icon-lg me-3"></AlertCircle>
          <div v-html="$t('index.startup_errors')"></div>
        </div>
        <ul class="mb-3">
          <li v-for="v in fancyLogs.filter(x => x.level === 'Fatal')" :key="`${v.timestamp}-${v.value}`">{{v.value}}</li>
        </ul>
        <RouterLink class="btn btn-danger" to="/troubleshooting#logs">
          <FileText :size="18" class="icon"></FileText>
          View Logs
        </RouterLink>
      </div>
    </div>

    <!-- Needed Attention Section: Missing Permissions Alert -->
    <div class="alert alert-warning my-4" v-if="missingPermissions.length">
      <div class="d-flex align-items-center gap-3 mb-2">
        <AlertTriangle :size="24" class="icon"></AlertTriangle>
        <strong>{{ $t('index.permissions_missing_title') }}</strong>
      </div>
      <p>{{ $t('index.permissions_missing_desc') }}</p>
      <RouterLink class="btn btn-warning" to="/troubleshooting#permissions">
        {{ $t('index.permissions_review') }}
      </RouterLink>
    </div>

    <!-- Needed Attention Section: Virtual Gamepad Broker / Virtual Input Notice -->
    <div class="alert my-4" :class="virtualInputNotice.alertClass" v-if="virtualInputNotice">
      <div>
        <div class="d-flex align-items-center mb-3">
          <AlertTriangle v-if="virtualInputNotice.warning" :size="32" class="icon-lg me-3"></AlertTriangle>
          <Info v-else :size="32" class="icon-lg me-3"></Info>
          <div class="d-flex flex-column">
            <h5 class="mb-0">{{ $t(virtualInputNotice.title) }}</h5>
            <small v-if="virtualInputNotice.warning" class="text-body-secondary">{{ $t('index.virtualhid_attention_sub') }}</small>
          </div>
        </div>
        <ul class="mb-3">
          <li v-for="m in virtualInputNotice.messages" :key="m.key">{{ $t(m.key, m.values) }}</li>
        </ul>
        <RouterLink class="btn" :class="virtualInputNotice.buttonClass" :to="virtualInputNotice.to">
          <Wrench v-if="virtualInputNotice.chooseDriver" :size="18" class="icon"></Wrench>
          <Wrench v-else :size="18" class="icon"></Wrench>
          {{ $t(virtualInputNotice.action) }}
        </RouterLink>
      </div>
    </div>

    <!-- Client Pairing & Ecosystem Section -->
    <div class="card my-4 ares-card">
      <div class="card-body">
        <div class="d-flex justify-content-between align-items-center flex-wrap gap-2 mb-3">
          <div>
            <h2 class="h5 mb-1">{{ $t('index.client_ecosystem_title') }}</h2>
            <p class="text-muted small mb-0">{{ $t('index.client_ecosystem_desc') }}</p>
          </div>
          <RouterLink class="btn btn-primary btn-sm d-inline-flex align-items-center gap-1" to="/pin">
            <Key :size="14" class="icon"></Key>
            {{ $t('index.pair_with_pin') }}
          </RouterLink>
        </div>
        <div class="d-flex flex-wrap gap-2 mt-2">
          <a class="btn btn-outline-success btn-sm d-inline-flex align-items-center gap-1" href="https://github.com/ClassicOldSong/moonlight-android" target="_blank" rel="noopener noreferrer">
            <ExternalLink :size="14" class="icon"></ExternalLink>
            {{ $t('index.artemis_client') }}
          </a>
          <a class="btn btn-outline-secondary btn-sm d-inline-flex align-items-center gap-1" href="https://moonlight-stream.org" target="_blank" rel="noopener noreferrer">
            <ExternalLink :size="14" class="icon"></ExternalLink>
            {{ $t('index.moonlight_clients') }}
          </a>
        </div>
      </div>
    </div>

    <!-- Upstream Sunshine Baseline Section -->
    <div class="card my-4 ares-card">
      <div class="card-body">
        <div class="d-flex justify-content-between align-items-start flex-wrap gap-2 mb-3">
          <div>
            <h2 class="h5 mb-1">{{ $t('index.upstream_baseline_title') }}</h2>
            <p class="text-muted small mb-0">{{ $t('index.upstream_baseline_desc') }}</p>
          </div>
          <div v-if="githubVersion">
            <span class="badge bg-secondary-subtle text-secondary-emphasis">
              Upstream: {{ githubVersion.version }}
            </span>
          </div>
        </div>

        <!-- Upstream Checking State -->
        <div v-if="githubLoading" class="my-3 text-muted small">
          <Loader2 :size="16" class="icon me-1 fa-spin"></Loader2>
          {{ $t('index.loading_latest') }}
        </div>

        <!-- Upstream Error / Unavailable State -->
        <div v-else-if="githubError" class="alert alert-secondary my-3 small">
          <Info :size="16" class="icon me-1"></Info>
          {{ $t('index.upstream_baseline_unavailable') }}
        </div>

        <!-- Upstream Pre-release Available Alert -->
        <div v-else-if="notifyPreReleases && preReleaseBuildAvailable" class="alert alert-warning my-3">
          <div class="d-flex justify-content-between align-items-start flex-wrap gap-2 mb-2">
            <div class="d-flex align-items-center gap-2">
              <AlertCircle :size="18" class="icon text-warning"></AlertCircle>
              <div>
                <strong>{{ $t('index.upstream_new_pre_release') }}</strong>
                <span class="badge bg-warning-subtle text-warning-emphasis ms-2">{{ preReleaseVersion.release.name }}</span>
              </div>
            </div>
            <a class="btn btn-sm btn-success flex-shrink-0 d-inline-flex align-items-center gap-1"
               :href="preReleaseVersion.release.html_url" target="_blank" rel="noopener noreferrer">
              <Download :size="14" class="icon"></Download>
              {{ $t('index.upstream_download') }}
            </a>
          </div>
          <div class="small text-muted mb-2">
            <em>{{ $t('index.upstream_note') }}</em>
          </div>
          <div v-if="preReleaseVersion.release.body" class="p-2 border rounded bg-body-tertiary small markdown-body"
               v-html="convertMarkdownToHtml(preReleaseVersion.release.body)">
          </div>
        </div>

        <!-- Upstream Stable Available Alert -->
        <div v-else-if="stableBuildAvailable" class="alert alert-warning my-3">
          <div class="d-flex justify-content-between align-items-start flex-wrap gap-2 mb-2">
            <div class="d-flex align-items-center gap-2">
              <AlertCircle :size="18" class="icon text-warning"></AlertCircle>
              <div>
                <strong>{{ $t('index.upstream_new_stable') }}</strong>
                <span class="badge bg-warning-subtle text-warning-emphasis ms-2">{{ githubVersion.release.name }}</span>
              </div>
            </div>
            <a class="btn btn-sm btn-success flex-shrink-0 d-inline-flex align-items-center gap-1"
               :href="githubVersion.release.html_url" target="_blank" rel="noopener noreferrer">
              <Download :size="14" class="icon"></Download>
              {{ $t('index.upstream_download') }}
            </a>
          </div>
          <div class="small text-muted mb-2">
            <em>{{ $t('index.upstream_note') }}</em>
          </div>
          <div v-if="githubVersion.release.body" class="p-2 border rounded bg-body-tertiary small markdown-body"
               v-html="convertMarkdownToHtml(githubVersion.release.body)">
          </div>
        </div>

        <!-- Upstream Baseline Current -->
        <div v-else class="alert alert-success my-3 small d-flex align-items-center gap-2">
          <CheckCircle :size="16" class="icon text-success"></CheckCircle>
          <span>{{ $t('index.upstream_baseline_current') }} ({{ githubVersion ? githubVersion.version : (version ? version.version : '') }})</span>
        </div>
      </div>
    </div>

    <!-- Upstream Resources & Project Credits / License -->
    <ResourceCard :installed-version-not-stable="installedVersionNotStable"></ResourceCard>

  </div>
</template>

<script>
  import { marked } from 'marked'
  import Navbar from './Navbar.vue'
  import ResourceCard from './ResourceCard.vue'
  import SunshineVersion from './sunshine_version'
  import {
    AlertCircle,
    AlertTriangle,
    CheckCircle,
    Cpu,
    Download,
    ExternalLink,
    FileText,
    Gamepad2,
    GitBranch,
    Info,
    Key,
    Loader2,
    Package,
    Wrench,
  } from '@lucide/vue'

  export default {
    name: 'Home',
    components: {
      Navbar,
      ResourceCard,
      AlertCircle,
      AlertTriangle,
      CheckCircle,
      Cpu,
      Download,
      ExternalLink,
      FileText,
      Gamepad2,
      GitBranch,
      Info,
      Key,
      Loader2,
      Package,
      Wrench,
    },
    data() {
      return {
        version: null,
        githubVersion: null,
        notifyPreReleases: false,
        preReleaseVersion: null,
        loading: true,
        configLoading: true,
        configError: false,
        githubLoading: true,
        githubError: false,
        logs: null,
        platform: "",
        controllerEnabled: false,
        gamepadDriver: '',
        virtualhid: null,
        virtualhidLicense: null,
        vigembus: null,
        permissions: [],
        pairedClientsCount: null,
      }
    },
    computed: {
      platformDisplay() {
        if (!this.platform) return '—'
        if (this.platform === 'windows') return 'Windows'
        if (this.platform === 'macos') return 'macOS'
        if (this.platform === 'linux') return 'Linux'
        return this.platform.charAt(0).toUpperCase() + this.platform.slice(1)
      },
      gamepadDriverDisplay() {
        if (!this.controllerEnabled) return ''
        if (this.gamepadDriver === 'virtualhid') return 'Virtual HID'
        if (this.gamepadDriver === 'vigembus') return 'ViGEmBus'
        if (this.gamepadDriver === 'none') return ''
        return this.gamepadDriver || ''
      },
      missingPermissions() {
        return this.permissions.filter(permission => permission.required && permission.verifiable && permission.status !== 'granted')
      },
      /**
       * Build the virtual-input message shown on the home page.
       * Warn about broker or gamepad driver issues when virtual gamepads are enabled.
       */
      virtualInputNotice() {
        if (!this.controllerEnabled || this.gamepadDriver === 'none') {
          return null
        }

        if (this.platform === 'macos') {
          return this.buildMacosVirtualInputNotice()
        }

        if (this.platform !== 'windows' || !this.virtualhid || !this.vigembus) {
          return null
        }

        const vigembusUsable = this.vigembus.installed && this.vigembus.version_compatible

        if (!this.gamepadDriver) {
          return this.buildVirtualInputNotice(false, 'index.gamepad_driver_choice_title', [{
            key: 'index.gamepad_driver_choice_desc',
          }], {
            action: 'index.choose_gamepad_driver',
            chooseDriver: true,
            to: '/config#gamepad_driver',
          })
        }

        if (this.gamepadDriver === 'vigembus') {
          return this.buildVigembusNotice(vigembusUsable)
        }

        if (this.virtualhid.installed) {
          return this.buildInstalledVirtualhidNotice(vigembusUsable)
        }

        if (this.gamepadDriver === 'virtualhid') {
          return this.buildVirtualInputNotice(true, 'index.virtualhid_broker_unavailable_title', [{ key: 'index.virtualhid_required_desc' }])
        }

        if (this.controllerEnabled && !vigembusUsable) {
          return this.buildVirtualInputNotice(true, 'index.virtual_input_unavailable_title', [{ key: 'index.virtual_input_unavailable_desc' }])
        }

        return this.buildVirtualInputNotice(
          false,
          'index.virtualhid_optional_title',
          [{ key: this.controllerEnabled ? 'index.virtualhid_optional_vigembus_desc' : 'index.virtualhid_optional_desc' }],
        )
      },
      installedVersionNotStable() {
        if (!this.githubVersion || !this.version) {
          return false
        }
        return this.version.isGreater(this.githubVersion)
      },
      stableBuildAvailable() {
        if (!this.githubVersion || !this.version) {
          return false
        }
        return this.githubVersion.isGreater(this.version)
      },
      preReleaseBuildAvailable() {
        if (!this.preReleaseVersion || !this.githubVersion || !this.version) {
          return false
        }
        return this.preReleaseVersion.isGreater(this.version) && this.preReleaseVersion.isGreater(this.githubVersion)
      },
      buildVersionIsDirty() {
        return this.version?.version?.split('.').length === 5 &&
          this.version.version.includes('dirty')
      },
      /** Parse the text errors, calculating the text, the timestamp and the level */
      fancyLogs() {
        if (!this.logs) return []
        const regex = /(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}]):\s/g
        const rawLogLines = (this.logs.split(regex)).splice(1)
        const logLines = []
        for (let i = 0; i < rawLogLines.length; i += 2) {
          logLines.push({ timestamp: rawLogLines[i], level: rawLogLines[i + 1].split(':')[0], value: rawLogLines[i + 1] })
        }
        return logLines
      },
    },
    async created() {
      console.log('Hello, Ares!')
      try {
        const config = await fetch('./api/config').then((r) => r.json())
        this.notifyPreReleases = config.notify_pre_releases
        this.platform = config.platform
        this.controllerEnabled = config.controller !== 'disabled'
        this.gamepadDriver = config.gamepad_driver || ''
        this.version = new SunshineVersion(null, config.version)
        console.log('Version: ', this.version.version)
      } catch (e) {
        console.error('Failed to fetch configuration:', e)
        this.configError = true
      } finally {
        this.configLoading = false
      }

      if (!this.configError) {
        try {
          const response = await fetch('./api/permissions')
          this.permissions = (await response.json()).permissions || []
        } catch (e) {
          console.error('Failed to fetch permission status:', e)
        }

        if (this.platform === 'windows' || this.platform === 'macos') {
          try {
            const virtualInputStatus = await fetch('./api/virtual-input/status').then((r) => r.json())
            this.virtualhid = virtualInputStatus.virtualhid
            if (this.platform === 'windows') {
              this.vigembus = virtualInputStatus.vigembus
            }
          } catch (e) {
            console.error('Failed to fetch virtual input driver status:', e)
          }
        }

        if (this.platform === 'windows' || this.platform === 'macos') {
          try {
            this.virtualhidLicense = await fetch('./api/virtual-input/license').then((r) => r.json())
          } catch (e) {
            console.error('Failed to fetch Virtual HID Broker license status:', e)
          }
        }

        try {
          const clientsRes = await fetch('./api/clients/list')
          if (clientsRes) {
            const clientsData = await clientsRes.json()
            if (clientsData && Array.isArray(clientsData.named_certs)) {
              this.pairedClientsCount = clientsData.named_certs.length
            }
          }
        } catch (e) {
          // Client count optional
        }
      }

      try {
        this.logs = (await fetch('./api/logs').then(r => r.text()))
      } catch (e) {
        console.error('Failed to fetch logs:', e)
      }

      // Upstream Sunshine release check (isolated and independent)
      try {
        const latestRelease = await fetch('https://api.github.com/repos/LizardByte/Sunshine/releases/latest').then((r) => r.json())
        if (latestRelease && latestRelease.tag_name) {
          this.githubVersion = new SunshineVersion(latestRelease, null)
          console.log('GitHub Version: ', this.githubVersion.version)
        } else {
          this.githubError = true
        }
      } catch (e) {
        console.error('Failed to fetch upstream Sunshine latest release:', e)
        this.githubError = true
      }

      try {
        const releasesList = await fetch('https://api.github.com/repos/LizardByte/Sunshine/releases').then((r) => r.json())
        if (Array.isArray(releasesList)) {
          const prerelease = releasesList.find(release => release && release.prerelease)
          if (prerelease && prerelease.tag_name) {
            this.preReleaseVersion = new SunshineVersion(prerelease, null)
            console.log('Pre-Release Version: ', this.preReleaseVersion.version)
          }
        }
      } catch (e) {
        console.error('Failed to fetch upstream Sunshine pre-releases:', e)
      } finally {
        this.githubLoading = false
        this.loading = false
      }
    },
    methods: {
      /**
       * Build the macOS broker notice, prioritizing license and service warnings.
       *
       * @returns {object|null} Warning or development-build notice, when applicable.
       */
      buildMacosVirtualInputNotice() {
        if (this.virtualhidLicense && !this.virtualhidLicense.licensed) {
          return this.virtualhidLicense.service_available
            ? this.buildVirtualInputNotice(true, 'index.virtualhid_macos_license_title', [{ key: 'index.virtualhid_macos_license_desc' }])
            : this.buildVirtualInputNotice(true, 'index.virtualhid_broker_unavailable_title', [{ key: 'index.virtualhid_macos_broker_desc' }])
        }
        return this.virtualhid?.development_version
          ? this.buildVirtualInputNotice(false, 'index.virtualhid_development_title', [{ key: 'index.virtualhid_development_desc' }])
          : null
      },
      /**
       * Build a home-page notice for the current virtual-input state.
       *
       * @param {boolean} warning Whether the notice represents an actionable warning.
       * @param {string} title Localization key for the notice title.
       * @param {object[]} messages Localized message descriptors.
       * @param {object} options Optional action and destination overrides.
       * @returns {object} Notice data consumed by the template.
       */
      buildVirtualInputNotice(warning, title, messages, options = {}) {
        return {
          action: options.action || 'index.review_virtual_input',
          alertClass: warning ? 'alert-warning' : 'alert-info',
          buttonClass: warning ? 'btn-warning' : 'btn-info',
          chooseDriver: options.chooseDriver || false,
          to: options.to || '/troubleshooting#virtualhid',
          messages,
          title,
          warning,
        }
      },
      /**
       * Build the notice for an explicitly selected ViGEmBus backend.
       *
       * @param {boolean} vigembusUsable Whether ViGEmBus is installed and compatible.
       * @returns {object|null} Warning data, or no notice when ViGEmBus is usable.
       */
      buildVigembusNotice(vigembusUsable) {
        if (!this.controllerEnabled || vigembusUsable) {
          return null
        }
        return this.buildVirtualInputNotice(true, 'index.vigembus_required_title', [{
          key: this.vigembus.installed ? 'index.vigembus_outdated_desc' : 'index.vigembus_not_installed_desc',
          params: { version: this.vigembus.version, supported_versions: this.vigembus.supported_versions },
        }])
      },
      /**
       * Build the notice for an installed Virtual HID Driver.
       *
       * @param {boolean} vigembusUsable Whether ViGEmBus is available as a fallback.
       * @returns {object|null} Warning or informational data, or no notice when fully usable.
       */
      buildInstalledVirtualhidNotice(vigembusUsable) {
        if (!this.virtualhid.version_compatible) {
          return this.buildVirtualInputNotice(true, 'index.virtualhid_outdated_title', [{
            key: 'index.virtualhid_outdated_desc',
            params: { version: this.virtualhid.version, supported_versions: this.virtualhid.supported_versions },
          }])
        }

        if (this.virtualhidLicense && !this.virtualhidLicense.licensed) {
          const fallbackToVigembus = vigembusUsable && this.gamepadDriver !== 'virtualhid'
          return this.buildVirtualInputNotice(true, 'index.virtualhid_attention_title', [{
            key: fallbackToVigembus ? 'index.virtualhid_license_invalid_fallback_desc' : 'index.virtualhid_license_invalid_desc',
          }])
        }

        if (this.virtualhid.development_version && vigembusUsable) {
          return this.buildVirtualInputNotice(true, 'index.virtualhid_attention_title', [
            { key: 'index.virtualhid_development_desc' },
            { key: 'index.virtualhid_optional_vigembus_desc' },
          ])
        }
        if (this.virtualhid.development_version) {
          return this.buildVirtualInputNotice(false, 'index.virtualhid_development_title', [{ key: 'index.virtualhid_development_desc' }])
        }
        return null
      },
      convertMarkdownToHtml(markdown) {
        if (!markdown) return ''
        return marked.parse(markdown)
      },
    },
  }
</script>

<style scoped>
.ares-overview__header {
  border-bottom: 1px solid var(--bs-border-color);
  padding-bottom: 1rem;
}
.ares-overview__title {
  font-size: 1.75rem;
  font-weight: 700;
  margin-bottom: 0.25rem;
}
.ares-overview__desc {
  font-size: 0.95rem;
}

.ares-stat-card {
  border-radius: 12px;
  border: 1px solid var(--bs-border-color);
  background-color: var(--bs-body-bg);
}
.ares-stat-card .card-body {
  padding: 1.25rem 1.5rem;
}
.ares-stat-card__label {
  font-size: 0.75rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: var(--bs-secondary-color);
  margin-bottom: 0.35rem;
  display: flex;
  align-items: center;
}
.ares-stat-card__value {
  font-size: 1.6rem;
  font-weight: 700;
  line-height: 1.2;
  font-variant-numeric: tabular-nums;
  margin-bottom: 0.4rem;
}
.ares-stat-card__sub {
  font-size: 0.825rem;
}

.ares-card {
  border-radius: 12px;
  border: 1px solid var(--bs-border-color);
}

.markdown-body {
  max-height: 250px;
  overflow-y: auto;
}
</style>
