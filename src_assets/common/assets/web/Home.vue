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
                {{ versionDisplay }}
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
              <span v-else-if="driverStatusState === 'unavailable'" class="badge bg-danger-subtle text-danger-emphasis">
                {{ $t('index.stat_driver_unavailable') }}
              </span>
              <span v-else-if="driverStatusState === 'unknown'" class="badge bg-secondary-subtle text-secondary-emphasis">
                {{ $t('index.client_count_not_reported') }}
              </span>
              <span v-else-if="driverStatusState === 'ready'" class="badge bg-success-subtle text-success-emphasis">
                <CheckCircle :size="12" class="icon me-1"></CheckCircle>
                {{ $t('index.stat_driver_ready') }}
              </span>
              <span v-else class="badge bg-secondary-subtle text-secondary-emphasis">
                {{ $t('index.stat_driver_unavailable') }}
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
                <span v-if="clientsError" class="text-muted fs-6">{{ $t('index.client_count_not_reported') }}</span>
                <span v-else-if="pairedClientsCount !== null">{{ pairedClientsCount }} {{ $t('index.paired_clients') }}</span>
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
          <a class="btn btn-outline-primary btn-sm d-inline-flex align-items-center gap-1" href="https://github.com/lupixele/Athena" target="_blank" rel="noopener noreferrer">
            <ExternalLink :size="14" class="icon"></ExternalLink>
            {{ $t('index.athena_client') }}
          </a>
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
        <div v-else-if="githubError || (!preReleaseVersion && !githubVersion)" class="alert alert-secondary my-3 small">
          <Info :size="16" class="icon me-1"></Info>
          {{ $t('index.upstream_baseline_unavailable') }}
        </div>

        <!-- Upstream Pre-release Available Alert -->
        <div v-else-if="notifyPreReleases && preReleaseVersion" class="alert alert-warning my-3">
          <div class="d-flex justify-content-between align-items-start flex-wrap gap-2 mb-2">
            <div class="d-flex align-items-center gap-2">
              <AlertCircle :size="18" class="icon text-warning"></AlertCircle>
              <div>
                <strong>{{ $t('index.upstream_new_pre_release') }}</strong>
                <span class="badge bg-warning-subtle text-warning-emphasis ms-2">{{ preReleaseVersion.release.name || preReleaseVersion.version }}</span>
              </div>
            </div>
            <a class="btn btn-sm btn-success flex-shrink-0 d-inline-flex align-items-center gap-1"
               :href="getSafeReleaseUrl(preReleaseVersion.release.html_url)" target="_blank" rel="noopener noreferrer">
              <Download :size="14" class="icon"></Download>
              {{ $t('index.upstream_download') }}
            </a>
          </div>
          <div class="small text-muted mb-2">
            <em>{{ $t('index.upstream_note') }}</em>
          </div>
          <div v-if="preReleaseVersion.release.body" class="p-2 border rounded bg-body-tertiary small markdown-body font-monospace text-start"
               style="white-space: pre-wrap;">{{ preReleaseVersion.release.body }}</div>
        </div>

        <!-- Upstream Latest Release Alert -->
        <div v-else-if="githubVersion" class="alert alert-warning my-3">
          <div class="d-flex justify-content-between align-items-start flex-wrap gap-2 mb-2">
            <div class="d-flex align-items-center gap-2">
              <AlertCircle :size="18" class="icon text-warning"></AlertCircle>
              <div>
                <strong>{{ $t('index.upstream_new_stable') }}</strong>
                <span class="badge bg-warning-subtle text-warning-emphasis ms-2">{{ githubVersion.release.name || githubVersion.version }}</span>
              </div>
            </div>
            <a class="btn btn-sm btn-success flex-shrink-0 d-inline-flex align-items-center gap-1"
               :href="getSafeReleaseUrl(githubVersion.release.html_url)" target="_blank" rel="noopener noreferrer">
              <Download :size="14" class="icon"></Download>
              {{ $t('index.upstream_download') }}
            </a>
          </div>
          <div class="small text-muted mb-2">
            <em>{{ $t('index.upstream_note') }}</em>
          </div>
          <div v-if="githubVersion.release.body" class="p-2 border rounded bg-body-tertiary small markdown-body font-monospace text-start"
               style="white-space: pre-wrap;">{{ githubVersion.release.body }}</div>
        </div>
      </div>
    </div>

    <!-- Upstream Resources & Project Credits / License -->
    <ResourceCard :installed-version-not-stable="Boolean(buildVersionIsDirty)"></ResourceCard>

  </div>
</template>

<script>
  import Navbar from './Navbar.vue'
  import ResourceCard from './ResourceCard.vue'
  import SunshineVersion from './sunshine_version'
  import { safeFetch } from './fetch_utils'
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
        rawVersion: '',
        githubVersion: null,
        notifyPreReleases: false,
        preReleaseVersion: null,
        loading: true,
        configLoading: true,
        configError: false,
        githubLoading: true,
        githubError: false,
        logs: null,
        logsError: false,
        platform: '',
        controllerEnabled: false,
        gamepadDriver: '',
        virtualhid: null,
        virtualhidLicense: null,
        vigembus: null,
        permissions: [],
        permissionsError: false,
        permissionsLoading: true,
        pairedClientsCount: null,
        clientsError: false,
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
      versionDisplay() {
        return this.rawVersion || this.version?.version || '—'
      },
      driverStatusState() {
        if (this.configLoading) return 'loading'
        if (this.configError) return 'offline'
        if (!this.controllerEnabled) return 'disabled'
        if (this.permissionsLoading) return 'unknown'

        if (this.fancyLogs.some(x => x.level === 'Fatal')) return 'unavailable'
        if (this.missingPermissions.length > 0 || this.permissionsError) return 'unavailable'

        if (this.platform === 'windows') {
          if (!this.gamepadDriver || this.gamepadDriver === 'none') {
            return 'unavailable'
          }
          if (this.gamepadDriver === 'virtualhid') {
            const vhidOk = Boolean(this.virtualhid?.installed && this.virtualhid?.version_compatible)
            const licOk = Boolean(this.virtualhidLicense?.licensed === true && this.virtualhidLicense?.service_available === true)
            return (vhidOk && licOk) ? 'ready' : 'unavailable'
          }
          if (this.gamepadDriver === 'vigembus') {
            const vigemOk = Boolean(this.vigembus?.installed && this.vigembus?.version_compatible)
            return vigemOk ? 'ready' : 'unavailable'
          }
          return 'unknown'
        }

        if (this.platform === 'macos') {
          if (!this.virtualhidLicense) return 'unknown'
          return this.virtualhidLicense.licensed === true && this.virtualhidLicense.service_available === true ? 'ready' : 'unavailable'
        }

        return 'unknown'
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
        return Boolean(this.buildVersionIsDirty)
      },
      stableBuildAvailable() {
        return false
      },
      preReleaseBuildAvailable() {
        return false
      },
      buildVersionIsDirty() {
        const v = this.rawVersion || this.version?.version
        return Boolean(v && v.split('.').length === 5 && v.includes('dirty'))
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
    created() {
      this.abortController = new AbortController()
      const signal = this.abortController.signal

      this.fetchLocalData(signal)
      this.fetchUpstreamData(signal)
    },
    unmounted() {
      if (this.abortController) {
        this.abortController.abort()
      }
    },
    methods: {
      getSafeReleaseUrl(url) {
        if (typeof url !== 'string' || !url.trim()) {
          return 'https://github.com/LizardByte/Sunshine/releases'
        }
        try {
          const parsed = new URL(url)
          if (parsed.protocol === 'https:' && parsed.hostname === 'github.com' && parsed.pathname.startsWith('/LizardByte/Sunshine/releases')) {
            return url
          }
        } catch (e) {
          // invalid URL
        }
        return 'https://github.com/LizardByte/Sunshine/releases'
      },
      async fetchLocalData(signal) {
        console.log('Hello, Ares!')
        try {
          const configRes = await safeFetch('./api/config', {
            signal, timeout: 5000,
            validate: data => data && typeof data === 'object' && !Array.isArray(data) && data.status !== false && typeof data.platform === 'string' && data.platform.length > 0 && typeof data.version === 'string' && data.version.length > 0,
          })
          if (!configRes.ok || !configRes.data) {
            this.configError = true
            this.configLoading = false
            this.clientsError = true
            return
          }
          const config = configRes.data
          this.notifyPreReleases = Boolean(config.notify_pre_releases)
          this.platform = config.platform || ''
          this.controllerEnabled = config.controller !== 'disabled'
          this.gamepadDriver = config.gamepad_driver || ''
          this.rawVersion = typeof config.version === 'string' ? config.version : ''
          try {
            this.version = this.rawVersion ? new SunshineVersion(null, this.rawVersion) : null
          } catch (e) {
            this.version = null
          }
          console.log('Version: ', this.rawVersion)
          this.configLoading = false
        } catch (e) {
          console.error('Failed to fetch configuration:', e)
          this.configError = true
          this.configLoading = false
          this.clientsError = true
          return
        }

        // Diagnostic queries based on config: independent and parallel
        safeFetch('./api/logs', { type: 'text', signal, timeout: 5000 }).then(res => {
          if (res.ok) {
            this.logs = res.data
          }
        })

        if (!this.configError) {
          safeFetch('./api/permissions', { signal, timeout: 5000, validate: data => Array.isArray(data?.permissions) }).then(res => {
            this.permissionsLoading = false
            if (res.ok && res.data) {
              this.permissions = res.data.permissions || []
            } else {
              this.permissionsError = true
            }
          }).catch(() => {
            this.permissionsLoading = false
            this.permissionsError = true
          })

          if (this.platform === 'windows' || this.platform === 'macos') {
            safeFetch('./api/virtual-input/status', { signal, timeout: 5000 }).then(res => {
              if (res.ok && res.data) {
                this.virtualhid = res.data.virtualhid || null
                if (this.platform === 'windows') {
                  this.vigembus = res.data.vigembus || null
                }
              }
            })

            safeFetch('./api/virtual-input/license', { signal, timeout: 5000, validate: data => typeof data?.licensed === 'boolean' && typeof data?.service_available === 'boolean' }).then(res => {
              if (res.ok && res.data) {
                this.virtualhidLicense = res.data
              }
            })
          }

          safeFetch('./api/clients/list', { signal, timeout: 5000 }).then(res => {
            if (res.ok && res.data?.status !== false && Array.isArray(res.data?.named_certs)) {
              this.pairedClientsCount = res.data.named_certs.length
              this.clientsError = false
            } else {
              this.clientsError = true
            }
          }).catch(() => {
            this.clientsError = true
          })
        }
      },
      async fetchUpstreamData(signal) {
        this.githubLoading = true
        this.githubError = false
        try {
          const latestPromise = safeFetch('https://api.github.com/repos/LizardByte/Sunshine/releases/latest', {
            signal,
            timeout: 5000,
            validate: data => data && typeof data.tag_name === 'string' && !data.message,
          })
          const releasesPromise = safeFetch('https://api.github.com/repos/LizardByte/Sunshine/releases', {
            signal,
            timeout: 5000,
            validate: data => Array.isArray(data),
          })

          const [latestRes, releasesRes] = await Promise.allSettled([latestPromise, releasesPromise])
          let foundAny = false

          if (latestRes.status === 'fulfilled' && latestRes.value.ok && latestRes.value.data) {
            try {
              this.githubVersion = new SunshineVersion(latestRes.value.data, null)
              console.log('GitHub Version: ', this.githubVersion.version)
              foundAny = true
            } catch (e) {
              this.githubVersion = null
            }
          }

          if (releasesRes.status === 'fulfilled' && releasesRes.value.ok && Array.isArray(releasesRes.value.data)) {
            const preRelease = releasesRes.value.data.find(r => r && r.prerelease && typeof r.tag_name === 'string')
            if (preRelease) {
              try {
                this.preReleaseVersion = new SunshineVersion(preRelease, null)
                console.log('Pre-Release Version: ', this.preReleaseVersion.version)
                foundAny = true
              } catch (e) {
                this.preReleaseVersion = null
              }
            }
          }

          if (!foundAny) {
            this.githubError = true
          }
        } catch (e) {
          this.githubError = true
        } finally {
          this.githubLoading = false
          this.loading = false
        }
      },
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
