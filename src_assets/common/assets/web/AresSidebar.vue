<template>
  <!-- Mobile overlay backdrop -->
  <div
    v-if="mobileOpen"
    class="ares-sidebar-backdrop"
    @click="mobileOpen = false"
    aria-hidden="true"
  />

  <!-- Sidebar -->
  <aside
    ref="sidebarEl"
    class="ares-sidebar"
    :class="{
      'ares-sidebar--collapsed': collapsed,
      'ares-sidebar--mobile-open': mobileOpen,
    }"
    :aria-label="$t('navbar.navigation')"
    :inert="isMobile && !mobileOpen ? true : undefined"
  >
    <!-- Logo / brand row -->
    <div class="ares-sidebar__header">
      <RouterLink class="ares-sidebar__brand" to="/" :title="$t('navbar.brand')">
        <img :src="logoSrc" height="32" alt="Ares" class="ares-sidebar__logo" />
        <span class="ares-sidebar__brand-name">Ares</span>
      </RouterLink>
      <button
        class="ares-sidebar__collapse-btn"
        @click="collapsed = !collapsed"
        :aria-label="collapsed ? $t('navbar.expand') : $t('navbar.collapse')"
        :aria-expanded="!collapsed"
        type="button"
      >
        <svg
          xmlns="http://www.w3.org/2000/svg"
          width="16" height="16"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          stroke-width="2"
          stroke-linecap="round"
          stroke-linejoin="round"
          class="ares-sidebar__chevron"
          :class="{ 'ares-sidebar__chevron--flipped': collapsed }"
          aria-hidden="true"
        >
          <polyline points="15 18 9 12 15 6" />
        </svg>
      </button>
    </div>

    <!-- Nav sections -->
    <nav class="ares-sidebar__nav" :aria-label="$t('navbar.navigation')">
      <ul class="ares-sidebar__section">
        <li v-for="item in navItems" :key="item.to" class="ares-sidebar__item-wrapper">
          <span
            class="ares-sidebar__active-bar"
            :class="{ 'ares-sidebar__active-bar--visible': isRouteActive(item) }"
            aria-hidden="true"
          />
          <RouterLink
            :to="item.to"
            class="ares-sidebar__item"
            :class="{ 'ares-sidebar__item--active': isRouteActive(item) }"
            :aria-current="isRouteActive(item) ? 'page' : undefined"
            :title="collapsed ? $t(item.labelKey) : undefined"
          >
            <span class="ares-sidebar__icon" aria-hidden="true">
              <component :is="item.icon" :size="18" />
            </span>
            <span class="ares-sidebar__label">{{ $t(item.labelKey) }}</span>
          </RouterLink>
        </li>
      </ul>
    </nav>

    <!-- Footer / theme / logout -->
    <div class="ares-sidebar__footer">
      <!-- Theme toggle -->
      <div class="ares-sidebar__theme-wrapper" :title="collapsed ? $t('navbar.toggle_theme') : undefined">
        <ThemeToggle />
      </div>

      <!-- Logout -->
      <button
        type="button"
        class="ares-sidebar__item ares-sidebar__logout-btn text-danger"
        @click="logout"
        :title="collapsed ? $t('navbar.logout') : undefined"
        :aria-label="$t('navbar.logout')"
      >
        <span class="ares-sidebar__icon" aria-hidden="true">
          <LogOut :size="18" />
        </span>
        <span class="ares-sidebar__label">{{ $t('navbar.logout') }}</span>
      </button>
    </div>
  </aside>

  <!-- Mobile hamburger trigger (fixed, outside sidebar) -->
  <button
    class="ares-sidebar__mobile-trigger"
    :class="{ 'ares-sidebar__mobile-trigger--open': mobileOpen }"
    @click="mobileOpen = !mobileOpen"
    :aria-label="$t('navbar.toggle_menu')"
    :aria-expanded="mobileOpen"
    type="button"
  >
    <svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24"
         fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"
         aria-hidden="true">
      <line x1="3" y1="6" x2="21" y2="6"/>
      <line x1="3" y1="12" x2="21" y2="12"/>
      <line x1="3" y1="18" x2="21" y2="18"/>
    </svg>
  </button>

  <!-- Notification toasts container mounted with sidebar -->
  <Notification />
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import {
  Home,
  Info,
  Layers,
  Lock,
  LogOut,
  Settings,
  Shield,
  Star,
} from '@lucide/vue'
import ThemeToggle from './ThemeToggle.vue'
import Notification from './Notification.vue'
import { logoutWithBasicAuth } from './logout.js'

const logoSrc = '/images/logo-apollo-45.png'

// ── Storage guard for collapse preference ──
const COLLAPSE_KEY = 'ares-sidebar-collapsed'

function lsGet(key) {
  try {
    if (typeof window !== 'undefined' && window.localStorage) {
      return window.localStorage.getItem(key)
    }
  } catch {
    // Bad storage / private mode / security error
  }
  return null
}

function lsSet(key, val) {
  try {
    if (typeof window !== 'undefined' && window.localStorage) {
      window.localStorage.setItem(key, val)
    }
  } catch {
    // Storage quota exceeded or disabled
  }
}

// ── Reactive router references ──
let router = null
let route = null
try {
  router = useRouter()
} catch {}
try {
  route = useRoute()
} catch {}

const currentPath = computed(() => {
  if (route && route.path) {
    return route.path
  }
  if (router && router.currentRoute && router.currentRoute.value) {
    return router.currentRoute.value.path
  }
  if (typeof window !== 'undefined' && window.location) {
    return window.location.pathname || '/'
  }
  return '/'
})

// ── State ──
const collapsed = ref(lsGet(COLLAPSE_KEY) === 'true')
const mobileOpen = ref(false)
const isMobile = ref(false)
const sidebarEl = ref(null)
let _lastFocusBeforeOpen = null
let originalBodyOverflow = ''
let originalContentInert = false

// ── Navigation items ──
const navItems = [
  { to: '/',              labelKey: 'navbar.home',          icon: Home },
  { to: '/pin',           labelKey: 'navbar.pin',           icon: Lock },
  { to: '/apps',          labelKey: 'navbar.applications',  icon: Layers },
  { to: '/featured',      labelKey: 'navbar.featured',      icon: Star },
  { to: '/config',        labelKey: 'navbar.configuration', icon: Settings },
  { to: '/troubleshooting', labelKey: 'navbar.troubleshoot', icon: Info },
  { to: '/password',      labelKey: 'navbar.password',      icon: Shield },
]

function isRouteActive(item) {
  const path = currentPath.value
  if (item.to === '/') {
    return path === '/'
  }
  return path === item.to || path.startsWith(item.to + '/')
}

// ── Viewport & Mobile ──
function updateMobile() {
  if (typeof window !== 'undefined' && window.matchMedia) {
    try {
      isMobile.value = window.matchMedia('(max-width: 768px)').matches
    } catch {
      isMobile.value = false
    }
  } else {
    isMobile.value = false
  }
}

// ── Sync collapse class on root & storage ──
watch(collapsed, (val) => {
  lsSet(COLLAPSE_KEY, String(val))
  if (typeof document !== 'undefined') {
    document.documentElement.classList.toggle('ares-sidebar-expanded', !val)
  }
})

// ── Mobile drawer: focus trap, scroll lock, inert background ──
function getVisibleFocusable(container) {
  if (!container) return []
  const elements = container.querySelectorAll(
    'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'
  )
  return Array.from(elements).filter(el => {
    if (el.disabled || el.hidden || el.closest('[hidden]')) return false
    const closedDropdown = el.closest('.dropdown-menu:not(.show)')
    if (closedDropdown) return false
    if (el.style.display === 'none' || el.style.visibility === 'hidden') return false
    return true
  })
}

function onKeyDown(e) {
  if (e.key === 'Escape') {
    const openDropdown = sidebarEl.value?.querySelector('.dropdown-menu.show')
    if (openDropdown) {
      openDropdown.classList.remove('show')
      const toggle = sidebarEl.value?.querySelector('[data-bs-toggle="dropdown"]')
      if (toggle) {
        toggle.setAttribute('aria-expanded', 'false')
        toggle.focus()
      }
      return
    }
    mobileOpen.value = false
    return
  }

  if (e.key === 'Tab' && mobileOpen.value && sidebarEl.value) {
    const focusable = getVisibleFocusable(sidebarEl.value)
    if (!focusable.length) return
    const first = focusable[0]
    const last = focusable[focusable.length - 1]

    if (e.shiftKey) {
      if (document.activeElement === first || !sidebarEl.value.contains(document.activeElement)) {
        e.preventDefault()
        last.focus()
      }
    } else if (mobileOpen.value) {
      if (document.activeElement === last || !sidebarEl.value.contains(document.activeElement)) {
        e.preventDefault()
        first.focus()
      }
    }
  }
}

watch(mobileOpen, (open) => {
  if (typeof document === 'undefined') return
  const contentEl = document.getElementById('content')

  if (open) {
    _lastFocusBeforeOpen = document.activeElement
    originalBodyOverflow = document.body.style.overflow
    originalContentInert = contentEl?.hasAttribute('inert') ?? false
    document.body.classList.add('ares-sidebar-scroll-locked')
    document.body.style.overflow = 'hidden'
    if (contentEl) {
      contentEl.setAttribute('inert', '')
    }
    if (typeof requestAnimationFrame !== 'undefined') {
      requestAnimationFrame(() => {
        const el = sidebarEl.value
        if (!el) return
        const focusable = getVisibleFocusable(el)
        if (focusable.length) focusable[0].focus()
      })
    }
  } else {
    document.body.classList.remove('ares-sidebar-scroll-locked')
    document.body.style.overflow = originalBodyOverflow
    if (contentEl) {
      contentEl.toggleAttribute('inert', originalContentInert)
    }
    if (_lastFocusBeforeOpen && typeof _lastFocusBeforeOpen.focus === 'function') {
      _lastFocusBeforeOpen.focus()
    }
    _lastFocusBeforeOpen = null
  }
})

// Close mobile drawer on route change
if (route) {
  watch(() => route.path, () => {
    mobileOpen.value = false
  })
}

// ── BasicAuth Logout ──
function logout() {
  logoutWithBasicAuth()
}

// ── Lifecycle ──
onMounted(() => {
  updateMobile()
  if (typeof window !== 'undefined') {
    window.addEventListener('resize', updateMobile, { passive: true })
    document.addEventListener('keydown', onKeyDown)
  }
  if (typeof document !== 'undefined') {
    originalBodyOverflow = document.body.style.overflow
    originalContentInert = document.getElementById('content')?.hasAttribute('inert') ?? false
    document.body.classList.add('has-ares-sidebar')
    document.documentElement.classList.add('has-ares-sidebar')
    document.documentElement.classList.toggle('ares-sidebar-expanded', !collapsed.value)
  }
})

onBeforeUnmount(() => {
  if (typeof window !== 'undefined') {
    window.removeEventListener('resize', updateMobile)
    document.removeEventListener('keydown', onKeyDown)
  }
  if (typeof document !== 'undefined') {
    document.body.classList.remove('has-ares-sidebar')
    document.body.classList.remove('ares-sidebar-scroll-locked')
    document.body.style.overflow = originalBodyOverflow
    document.documentElement.classList.remove('has-ares-sidebar')
    document.documentElement.classList.remove('ares-sidebar-expanded')
    const contentEl = document.getElementById('content')
    if (contentEl) {
      contentEl.toggleAttribute('inert', originalContentInert)
    }
  }
})

defineExpose({
  collapsed,
  mobileOpen,
  isMobile,
  logout,
  isRouteActive,
  navItems,
})
</script>

<style src="./ares-sidebar.css"></style>
