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
    class="ares-sidebar"
    :class="{
      'ares-sidebar--collapsed': collapsed,
      'ares-sidebar--mobile-open': mobileOpen,
    }"
    :aria-label="$t('navbar.navigation')"
  >
    <!-- Logo / brand row -->
    <div class="ares-sidebar__header">
      <a class="ares-sidebar__brand" href="./" :title="$t('navbar.brand')">
        <img src="/images/logo-apollo-45.png" height="32" alt="Ares" class="ares-sidebar__logo" />
        <span class="ares-sidebar__brand-name">Ares</span>
      </a>
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
    <nav class="ares-sidebar__nav" aria-label="Main navigation">
      <ul class="ares-sidebar__section">
        <li v-for="item in navItems" :key="item.href" class="ares-sidebar__item-wrapper">
          <a
            :href="item.href"
            class="ares-sidebar__item"
            :class="{ 'ares-sidebar__item--active': isActive(item) }"
            :aria-current="isActive(item) ? 'page' : undefined"
            :title="collapsed ? $t(item.labelKey) : undefined"
          >
            <!-- Active indicator bar -->
            <span class="ares-sidebar__active-bar" aria-hidden="true" />

            <!-- Icon -->
            <span class="ares-sidebar__icon" aria-hidden="true" v-html="item.icon" />

            <!-- Label -->
            <span class="ares-sidebar__label">{{ $t(item.labelKey) }}</span>
          </a>
        </li>
      </ul>
    </nav>

    <!-- Footer / theme -->
    <div class="ares-sidebar__footer">
      <div class="ares-sidebar__item ares-sidebar__theme-wrapper">
        <span class="ares-sidebar__icon" aria-hidden="true">
          <svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24"
               fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <circle cx="12" cy="12" r="4"/>
            <path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41"/>
          </svg>
        </span>
        <span class="ares-sidebar__label">
          <ThemeToggle />
        </span>
      </div>
    </div>
  </aside>

  <!-- Mobile hamburger trigger (fixed, outside sidebar) -->
  <button
    class="ares-sidebar__mobile-trigger"
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
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import ThemeToggle from './ThemeToggle.vue'

// Persist collapse state across page loads
const COLLAPSE_KEY = 'ares-sidebar-collapsed'
const storedCollapsed = localStorage.getItem(COLLAPSE_KEY)
const collapsed = ref(storedCollapsed === 'true')
const mobileOpen = ref(false)

watch(collapsed, (val) => {
  localStorage.setItem(COLLAPSE_KEY, String(val))
  document.documentElement.classList.toggle('ares-sidebar-expanded', !val)
})

// Set initial class on root
if (!collapsed.value) {
  document.documentElement.classList.add('ares-sidebar-expanded')
}

const currentPath = computed(() => {
  // Resolve current page from location
  const path = window.location.pathname
  const file = path.split('/').pop() || 'index'
  return file.replace('.html', '') || 'index'
})

function isActive(item) {
  return item.activeOn.includes(currentPath.value)
}

// SVG icon strings (inline, no external dependency)
const icons = {
  home: `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><polyline points="9 22 9 12 15 12 15 22"/></svg>`,
  apps: `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/></svg>`,
  config: `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>`,
  pin: `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>`,
  password: `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>`,
  troubleshoot: `<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>`,
}

const navItems = [
  { href: './',              labelKey: 'navbar.home',          icon: icons.home,         activeOn: ['index', ''] },
  { href: './apps',          labelKey: 'navbar.applications',  icon: icons.apps,         activeOn: ['apps'] },
  { href: './config',        labelKey: 'navbar.configuration', icon: icons.config,       activeOn: ['config'] },
  { href: './pin',           labelKey: 'navbar.pin',           icon: icons.pin,          activeOn: ['pin'] },
  { href: './password',      labelKey: 'navbar.password',      icon: icons.password,     activeOn: ['password'] },
  { href: './troubleshooting', labelKey: 'navbar.troubleshoot', icon: icons.troubleshoot, activeOn: ['troubleshooting'] },
]
</script>

<style scoped>
/* ─────────────────────────────────────────────────
   Ares Sidebar — icon-rail collapsible nav shell
   Charcoal Ink (dark) / Alabaster (light) palette
───────────────────────────────────────────────── */

/* CSS variables — light defaults, dark overrides via Bootstrap data-bs-theme or prefers-color-scheme */
.ares-sidebar {
  --ares-sidebar-bg:        #f8f9fa;
  --ares-sidebar-border:    #e3e6ea;
  --ares-sidebar-text:      #374151;
  --ares-sidebar-text-mute: #6b7280;
  --ares-sidebar-accent:    #4f6ef7;
  --ares-sidebar-active-bg: rgba(79, 110, 247, 0.08);
  --ares-sidebar-hover-bg:  rgba(0, 0, 0, 0.04);
  --ares-sidebar-width:     220px;
  --ares-sidebar-rail:      56px;
  --ares-sidebar-transition: 220ms cubic-bezier(0.4, 0, 0.2, 1);
  --ares-header-h:           52px;
}

/* Dark theme overrides — Bootstrap data-bs-theme toggle */
:global([data-bs-theme=dark]) .ares-sidebar {
  --ares-sidebar-bg:        #18181b;
  --ares-sidebar-border:    #27272a;
  --ares-sidebar-text:      #e4e4e7;
  --ares-sidebar-text-mute: #71717a;
  --ares-sidebar-accent:    #6d88f8;
  --ares-sidebar-active-bg: rgba(109, 136, 248, 0.12);
  --ares-sidebar-hover-bg:  rgba(255, 255, 255, 0.04);
}

/* Dark theme overrides — OS prefers-color-scheme */
@media (prefers-color-scheme: dark) {
  .ares-sidebar {
    --ares-sidebar-bg:        #18181b;
    --ares-sidebar-border:    #27272a;
    --ares-sidebar-text:      #e4e4e7;
    --ares-sidebar-text-mute: #71717a;
    --ares-sidebar-accent:    #6d88f8;
    --ares-sidebar-active-bg: rgba(109, 136, 248, 0.12);
    --ares-sidebar-hover-bg:  rgba(255, 255, 255, 0.04);
  }
}

/* Sidebar shell */
.ares-sidebar {
  position: fixed;
  top: 0;
  left: 0;
  bottom: 0;
  z-index: 1030;
  width: var(--ares-sidebar-width);
  background: var(--ares-sidebar-bg);
  border-right: 1px solid var(--ares-sidebar-border);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  transition: width var(--ares-sidebar-transition),
              transform var(--ares-sidebar-transition);
  will-change: width;
}

.ares-sidebar--collapsed {
  width: var(--ares-sidebar-rail);
}

/* ── Header ── */
.ares-sidebar__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  height: var(--ares-header-h);
  padding: 0 12px;
  border-bottom: 1px solid var(--ares-sidebar-border);
  flex-shrink: 0;
}

.ares-sidebar__brand {
  display: flex;
  align-items: center;
  gap: 10px;
  text-decoration: none;
  color: var(--ares-sidebar-text);
  font-weight: 700;
  font-size: 1.05rem;
  letter-spacing: 0.01em;
  min-width: 0;
}

.ares-sidebar__logo {
  flex-shrink: 0;
}

.ares-sidebar__brand-name {
  white-space: nowrap;
  overflow: hidden;
  transition: opacity var(--ares-sidebar-transition), max-width var(--ares-sidebar-transition);
  max-width: 140px;
  opacity: 1;
}

.ares-sidebar--collapsed .ares-sidebar__brand-name {
  max-width: 0;
  opacity: 0;
}

.ares-sidebar__collapse-btn {
  flex-shrink: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  border: 1px solid var(--ares-sidebar-border);
  border-radius: 6px;
  background: transparent;
  color: var(--ares-sidebar-text-mute);
  cursor: pointer;
  padding: 0;
  transition: background var(--ares-sidebar-transition), color var(--ares-sidebar-transition);
}

.ares-sidebar__collapse-btn:hover {
  background: var(--ares-sidebar-hover-bg);
  color: var(--ares-sidebar-text);
}

.ares-sidebar__chevron {
  transition: transform var(--ares-sidebar-transition);
}

.ares-sidebar__chevron--flipped {
  transform: rotate(180deg);
}

/* ── Nav ── */
.ares-sidebar__nav {
  flex: 1;
  overflow-y: auto;
  overflow-x: hidden;
  padding: 8px 0;
  scrollbar-width: thin;
  scrollbar-color: var(--ares-sidebar-border) transparent;
}

.ares-sidebar__section {
  list-style: none;
  margin: 0;
  padding: 0 8px;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.ares-sidebar__item-wrapper {
  position: relative;
}

.ares-sidebar__item {
  position: relative;
  display: flex;
  align-items: center;
  gap: 10px;
  height: 40px;
  padding: 0 8px;
  border-radius: 8px;
  color: var(--ares-sidebar-text);
  text-decoration: none;
  font-size: 0.875rem;
  font-weight: 500;
  white-space: nowrap;
  transition: background var(--ares-sidebar-transition), color var(--ares-sidebar-transition);
  overflow: hidden;
}

.ares-sidebar__item:hover {
  background: var(--ares-sidebar-hover-bg);
  color: var(--ares-sidebar-text);
  text-decoration: none;
}

.ares-sidebar__item--active {
  background: var(--ares-sidebar-active-bg);
  color: var(--ares-sidebar-accent);
}

.ares-sidebar__item--active:hover {
  background: var(--ares-sidebar-active-bg);
  color: var(--ares-sidebar-accent);
}

/* Animated active indicator bar on left edge */
.ares-sidebar__active-bar {
  position: absolute;
  left: -8px;
  top: 50%;
  transform: translateY(-50%) scaleY(0);
  width: 3px;
  height: 60%;
  border-radius: 0 3px 3px 0;
  background: var(--ares-sidebar-accent);
  transition: transform var(--ares-sidebar-transition);
}

.ares-sidebar__item--active .ares-sidebar__active-bar {
  transform: translateY(-50%) scaleY(1);
}

.ares-sidebar__icon {
  flex-shrink: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  width: 20px;
  height: 20px;
  color: inherit;
}

.ares-sidebar__label {
  overflow: hidden;
  white-space: nowrap;
  transition: opacity var(--ares-sidebar-transition), max-width var(--ares-sidebar-transition);
  max-width: 180px;
  opacity: 1;
}

.ares-sidebar--collapsed .ares-sidebar__label {
  max-width: 0;
  opacity: 0;
}

/* ── Footer ── */
.ares-sidebar__footer {
  flex-shrink: 0;
  padding: 8px;
  border-top: 1px solid var(--ares-sidebar-border);
}

.ares-sidebar__theme-wrapper {
  cursor: default;
}

.ares-sidebar__theme-wrapper:hover {
  background: var(--ares-sidebar-hover-bg);
}

/* ── Mobile: sidebar off-screen by default ── */
@media (max-width: 768px) {
  .ares-sidebar {
    transform: translateX(-100%);
    width: var(--ares-sidebar-width) !important;
  }

  .ares-sidebar--mobile-open {
    transform: translateX(0);
    box-shadow: 4px 0 24px rgba(0, 0, 0, 0.18);
  }
}

/* ── Mobile hamburger button ── */
.ares-sidebar__mobile-trigger {
  display: none;
  position: fixed;
  top: 10px;
  left: 10px;
  z-index: 1040;
  width: 36px;
  height: 36px;
  border-radius: 8px;
  border: 1px solid var(--ares-sidebar-border, #e3e6ea);
  background: var(--ares-sidebar-bg, #f8f9fa);
  color: var(--ares-sidebar-text, #374151);
  align-items: center;
  justify-content: center;
  padding: 0;
  cursor: pointer;
}

@media (max-width: 768px) {
  .ares-sidebar__mobile-trigger {
    display: flex;
  }
}

/* ── Backdrop ── */
.ares-sidebar-backdrop {
  position: fixed;
  inset: 0;
  z-index: 1025;
  background: rgba(0, 0, 0, 0.4);
  backdrop-filter: blur(2px);
}
</style>
