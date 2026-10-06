import { mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { createMemoryHistory, createRouter } from 'vue-router'
import fs from 'node:fs'
import path from 'node:path'

import AresSidebar from '../../src_assets/common/assets/web/AresSidebar.vue'
import Navbar from '../../src_assets/common/assets/web/Navbar.vue'

function createTestRouter(initialPath = '/') {
  const routes = [
    { path: '/', component: { template: '<div>Home</div>' } },
    { path: '/pin', component: { template: '<div>PIN</div>' } },
    { path: '/apps', component: { template: '<div>Apps</div>' } },
    { path: '/featured', component: { template: '<div>Featured</div>' } },
    { path: '/config', component: { template: '<div>Config</div>' } },
    { path: '/troubleshooting', component: { template: '<div>Troubleshooting</div>' } },
    { path: '/password', component: { template: '<div>Password</div>' } },
    { path: '/logout', component: { template: '<div>Logout</div>' } },
  ]
  const router = createRouter({
    history: createMemoryHistory(),
    routes,
  })
  router.push(initialPath)
  return router
}

const globalOptions = (router) => ({
  plugins: router ? [router] : [],
  mocks: {
    $t: (key) => key,
  },
  stubs: {
    ThemeToggle: {
      template: `
        <div class="dropdown bd-mode-toggle">
          <button id="bd-theme" data-bs-toggle="dropdown" aria-expanded="false" class="dropdown-toggle">Theme</button>
          <ul class="dropdown-menu theme-menu">
            <li><button class="dropdown-item">Auto</button></li>
          </ul>
        </div>
      `,
    },
    Notification: { template: '<div class="notification-stub" />' },
  },
})

describe('AresSidebar Component', () => {
  let contentEl

  beforeEach(() => {
    localStorage.clear()
    document.body.className = ''
    document.documentElement.className = ''
    document.body.style.overflow = ''
    contentEl = document.createElement('div')
    contentEl.id = 'content'
    document.body.appendChild(contentEl)
  })

  afterEach(() => {
    if (contentEl && contentEl.parentNode) {
      contentEl.parentNode.removeChild(contentEl)
    }
    vi.restoreAllMocks()
  })

  it('mounts properly and applies shell class to body and html', async () => {
    const router = createTestRouter('/')
    await router.isReady()
    const wrapper = mount(AresSidebar, { global: globalOptions(router) })

    expect(wrapper.find('aside.ares-sidebar').exists()).toBe(true)
    expect(document.body.classList.contains('has-ares-sidebar')).toBe(true)
    expect(document.documentElement.classList.contains('has-ares-sidebar')).toBe(true)
    expect(document.documentElement.classList.contains('ares-sidebar-expanded')).toBe(true)

    // Brand and items rendered
    expect(wrapper.find('.ares-sidebar__brand-name').text()).toBe('Ares')
    const links = wrapper.findAll('a.ares-sidebar__item')
    expect(links.length).toBe(7)

    wrapper.unmount()
    expect(document.body.classList.contains('has-ares-sidebar')).toBe(false)
    expect(document.documentElement.classList.contains('has-ares-sidebar')).toBe(false)
    expect(document.documentElement.classList.contains('ares-sidebar-expanded')).toBe(false)
  })

  describe('router active route and navigation', () => {
    it('highlights active route and responds to route changes', async () => {
      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { global: globalOptions(router) })

      const homeLink = wrapper.find('.ares-sidebar__nav a[href="/"]')
      expect(homeLink.classes()).toContain('ares-sidebar__item--active')
      expect(homeLink.attributes('aria-current')).toBe('page')

      const configLink = wrapper.find('.ares-sidebar__nav a[href="/config"]')
      expect(configLink.classes()).not.toContain('ares-sidebar__item--active')

      // Navigate to /config
      await router.push('/config')
      await wrapper.vm.$nextTick()

      expect(wrapper.find('.ares-sidebar__nav a[href="/config"]').classes()).toContain('ares-sidebar__item--active')
      expect(wrapper.find('.ares-sidebar__nav a[href="/"]').classes()).not.toContain('ares-sidebar__item--active')

      // Navigate to subpath of /troubleshooting
      await router.push('/troubleshooting')
      await wrapper.vm.$nextTick()
      expect(wrapper.find('.ares-sidebar__nav a[href="/troubleshooting"]').classes()).toContain('ares-sidebar__item--active')
    })
  })

  describe('collapse state & storage guard', () => {
    it('toggles collapsed state and persists in localStorage', async () => {
      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { global: globalOptions(router) })

      const collapseBtn = wrapper.find('button.ares-sidebar__collapse-btn')
      expect(collapseBtn.attributes('aria-expanded')).toBe('true')
      expect(collapseBtn.attributes('aria-label')).toBe('navbar.collapse')

      // Click collapse button
      await collapseBtn.trigger('click')
      expect(wrapper.find('aside').classes()).toContain('ares-sidebar--collapsed')
      expect(document.documentElement.classList.contains('ares-sidebar-expanded')).toBe(false)
      expect(localStorage.getItem('ares-sidebar-collapsed')).toBe('true')
      expect(collapseBtn.attributes('aria-expanded')).toBe('false')
      expect(collapseBtn.attributes('aria-label')).toBe('navbar.expand')

      // Click again to expand
      await collapseBtn.trigger('click')
      expect(wrapper.find('aside').classes()).not.toContain('ares-sidebar--collapsed')
      expect(document.documentElement.classList.contains('ares-sidebar-expanded')).toBe(true)
      expect(localStorage.getItem('ares-sidebar-collapsed')).toBe('false')
    })

    it('tolerates bad storage / private mode without throwing', async () => {
      vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
        throw new DOMException('SecurityError')
      })
      vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
        throw new DOMException('SecurityError')
      })

      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { global: globalOptions(router) })

      const collapseBtn = wrapper.find('button.ares-sidebar__collapse-btn')
      expect(() => collapseBtn.trigger('click')).not.toThrow()
      await wrapper.vm.$nextTick()
      expect(wrapper.vm.collapsed).toBe(true)
    })
  })

  describe('mobile drawer, backdrop, focus trap, and escape layer', () => {
    it('opens mobile drawer and backdrop, closes when backdrop is clicked', async () => {
      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { global: globalOptions(router) })

      const mobileTrigger = wrapper.find('button.ares-sidebar__mobile-trigger')
      expect(wrapper.find('.ares-sidebar-backdrop').exists()).toBe(false)

      await mobileTrigger.trigger('click')
      expect(wrapper.vm.mobileOpen).toBe(true)
      expect(wrapper.find('.ares-sidebar-backdrop').exists()).toBe(true)
      expect(wrapper.find('aside').classes()).toContain('ares-sidebar--mobile-open')
      expect(document.body.classList.contains('ares-sidebar-scroll-locked')).toBe(true)
      expect(document.body.style.overflow).toBe('hidden')
      expect(contentEl.hasAttribute('inert')).toBe(true)

      // Click backdrop to close
      await wrapper.find('.ares-sidebar-backdrop').trigger('click')
      expect(wrapper.vm.mobileOpen).toBe(false)
      expect(wrapper.find('.ares-sidebar-backdrop').exists()).toBe(false)
      expect(document.body.classList.contains('ares-sidebar-scroll-locked')).toBe(false)
      expect(document.body.style.overflow).toBe('')
      expect(contentEl.hasAttribute('inert')).toBe(false)
    })

    it('closes mobile drawer on route navigation', async () => {
      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { global: globalOptions(router) })

      wrapper.vm.mobileOpen = true
      await wrapper.vm.$nextTick()
      expect(wrapper.vm.mobileOpen).toBe(true)

      await router.push('/apps')
      await wrapper.vm.$nextTick()
      expect(wrapper.vm.mobileOpen).toBe(false)
    })

    it('handles layered Escape keydown: closes open dropdown first, then mobile drawer', async () => {
      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { attachTo: document.body, global: globalOptions(router) })

      wrapper.vm.mobileOpen = true
      await wrapper.vm.$nextTick()

      // Emulate open dropdown inside sidebar
      const themeMenu = wrapper.find('.theme-menu')
      themeMenu.element.classList.add('show')
      const themeToggleBtn = wrapper.find('#bd-theme')
      themeToggleBtn.element.setAttribute('aria-expanded', 'true')

      // First Escape: closes dropdown, mobile drawer remains open
      const escapeEvent1 = new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })
      document.dispatchEvent(escapeEvent1)
      await wrapper.vm.$nextTick()

      expect(themeMenu.element.classList.contains('show')).toBe(false)
      expect(wrapper.vm.mobileOpen).toBe(true)

      // Second Escape: closes mobile drawer
      const escapeEvent2 = new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })
      document.dispatchEvent(escapeEvent2)
      await wrapper.vm.$nextTick()

      expect(wrapper.vm.mobileOpen).toBe(false)
      wrapper.unmount()
    })

    it('cycles focus within visible sidebar elements on Tab and Shift+Tab', async () => {
      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { attachTo: document.body, global: globalOptions(router) })

      wrapper.vm.mobileOpen = true
      await wrapper.vm.$nextTick()

      const asideEl = wrapper.find('aside').element
      const focusable = asideEl.querySelectorAll('a[href], button:not([disabled])')
      expect(focusable.length).toBeGreaterThan(2)

      const firstEl = focusable[0]
      const lastEl = focusable[focusable.length - 1]

      // Shift+Tab from first element wraps to last
      firstEl.focus()
      const shiftTabEvent = new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, bubbles: true, cancelable: true })
      document.dispatchEvent(shiftTabEvent)
      expect(shiftTabEvent.defaultPrevented).toBe(true)
      expect(document.activeElement).toBe(lastEl)

      // Tab from last element wraps to first
      lastEl.focus()
      const tabEvent = new KeyboardEvent('keydown', { key: 'Tab', shiftKey: false, bubbles: true, cancelable: true })
      document.dispatchEvent(tabEvent)
      expect(tabEvent.defaultPrevented).toBe(true)
      expect(document.activeElement).toBe(firstEl)

      wrapper.unmount()
    })
  })

  describe('reduced motion & 44px target CSS audit', () => {
    it('verifies reduced motion media query and 44px touch targets in ares-sidebar.css', () => {
      const cssPath = path.resolve(__dirname, '../../src_assets/common/assets/web/ares-sidebar.css')
      const cssContent = fs.readFileSync(cssPath, 'utf8')

      // Reduced motion test
      expect(cssContent).toContain('@media (prefers-reduced-motion: reduce)')
      expect(cssContent).toContain('transition: none !important')

      // 44px touch target test
      expect(cssContent).toContain('min-height: 44px')
      expect(cssContent).toContain('width: 44px')
      expect(cssContent).toContain('height: 44px')
    })
  })

  describe('logout action', () => {
    it('sends sunshine-logout basic auth clearing request', async () => {
      const xhrMock = {
        open: vi.fn(),
        setRequestHeader: vi.fn(),
        send: vi.fn(),
        timeout: 0,
      }
      function MockXHR() {
        return xhrMock
      }
      vi.stubGlobal('XMLHttpRequest', vi.fn(MockXHR))

      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(AresSidebar, { global: globalOptions(router) })

      const logoutBtn = wrapper.find('button.ares-sidebar__logout-btn')
      await logoutBtn.trigger('click')

      expect(xhrMock.open).toHaveBeenCalledWith('GET', '/', true, 'sunshine-logout', expect.any(String))
      expect(xhrMock.setRequestHeader).toHaveBeenCalledWith('Cache-Control', 'no-store')
      expect(xhrMock.send).toHaveBeenCalled()
    })
  })

  describe('Navbar wrapper', () => {
    it('mounts Navbar and renders adapted AresSidebar and Notification', async () => {
      const router = createTestRouter('/')
      await router.isReady()
      const wrapper = mount(Navbar, { global: globalOptions(router) })

      expect(wrapper.findComponent(AresSidebar).exists()).toBe(true)
      expect(wrapper.find('.notification-stub').exists()).toBe(true)
    })
  })
})
