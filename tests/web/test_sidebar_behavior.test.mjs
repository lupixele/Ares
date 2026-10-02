import { test } from 'node:test';
import assert from 'node:assert/strict';

// Behavioral test for focus trap logic in AresSidebar
// Emulates a DOM container structure with active/disabled/hidden elements,
// testing focus boundary calculations and dropdown Escape layered hierarchy.

function mockElement(tag, attrs = {}) {
  const el = {
    tagName: tag.toUpperCase(),
    attributes: { ...attrs },
    offsetWidth: attrs.hidden ? 0 : 100,
    offsetHeight: attrs.hidden ? 0 : 20,
    getClientRects: () => (attrs.hidden ? [] : [{ width: 100, height: 20 }]),
    classList: {
      _classes: new Set(attrs.classes || []),
      contains(cls) { return this._classes.has(cls); },
      add(cls) { this._classes.add(cls); },
      remove(cls) { this._classes.delete(cls); }
    },
    disabled: !!attrs.disabled,
    focused: false,
    focus() {
      if (el.ownerDocument) el.ownerDocument.activeElement = el;
      el.focused = true;
    },
    click() {
      if (el.onclick) el.onclick();
    },
    getAttribute(name) { return this.attributes[name]; },
    setAttribute(name, val) { this.attributes[name] = val; }
  };
  return el;
}

function mockContainer(elements) {
  const doc = { activeElement: null };
  for (const el of elements) {
    el.ownerDocument = doc;
  }
  return {
    doc,
    elements,
    querySelectorAll(selector) {
      // Basic selector match emulating querySelectorAll
      return elements.filter(el => {
        if (selector.includes('a[href]') && el.tagName === 'A' && el.attributes.href) return true;
        if (selector.includes('button:not([disabled])') && el.tagName === 'BUTTON' && !el.disabled) return true;
        if (selector.includes('[tabindex]:not([tabindex="-1"])') && el.attributes.tabindex && el.attributes.tabindex !== '-1') return true;
        return false;
      });
    },
    querySelector(selector) {
      if (selector.includes('.dropdown-menu.show')) {
        return elements.find(el => el.classList.contains('dropdown-menu') && el.classList.contains('show')) || null;
      }
      if (selector.includes('[data-bs-toggle="dropdown"]')) {
        return elements.find(el => el.attributes['data-bs-toggle'] === 'dropdown') || null;
      }
      return null;
    },
    contains(target) {
      return elements.includes(target);
    }
  };
}

function getVisibleFocusable(container) {
  const elements = container.querySelectorAll(
    'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'
  );
  return Array.from(elements).filter(el => {
    return el.offsetWidth > 0 || el.offsetHeight > 0 || el.getClientRects().length > 0;
  });
}

function simulateTrapKeyDown(e, state, container) {
  if (e.key === 'Escape' && state.mobileOpen) {
    const openDropdown = container.querySelector('.dropdown-menu.show');
    if (openDropdown) {
      const toggle = container.querySelector('[data-bs-toggle="dropdown"][aria-expanded="true"]');
      if (toggle) {
        toggle.click();
        toggle.focus();
      } else {
        openDropdown.classList.remove('show');
      }
      return; // Do NOT close drawer!
    }
    state.mobileOpen = false;
    return;
  }

  if (e.key === 'Tab' && state.mobileOpen) {
    const focusable = getVisibleFocusable(container);
    if (!focusable.length) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (e.shiftKey) {
      if (container.doc.activeElement === first || !container.contains(container.doc.activeElement)) {
        e.preventDefault();
        last.focus();
      }
    } else {
      if (container.doc.activeElement === last || !container.contains(container.doc.activeElement)) {
        e.preventDefault();
        first.focus();
      }
    }
  }
}

test('[Behavioral] AresSidebar focus trap handles Tab cycles and includes visible elements', () => {
  const brand = mockElement('a', { href: './' });
  const nav1 = mockElement('a', { href: './apps.html' });
  const navHidden = mockElement('a', { href: './hidden.html', hidden: true });
  const disabledBtn = mockElement('button', { disabled: true });
  const themeToggle = mockElement('button', { 'data-bs-toggle': 'dropdown' });

  const container = mockContainer([brand, nav1, navHidden, disabledBtn, themeToggle]);
  const visible = getVisibleFocusable(container);

  // Hidden and disabled elements MUST NOT be returned in focus trap
  assert.equal(visible.length, 3);
  assert.equal(visible[0], brand);
  assert.equal(visible[1], nav1);
  assert.equal(visible[2], themeToggle);

  // Tab forward from last element wraps to first
  const state = { mobileOpen: true };
  themeToggle.focus();
  assert.equal(container.doc.activeElement, themeToggle);

  let prevented = false;
  simulateTrapKeyDown({ key: 'Tab', shiftKey: false, preventDefault: () => { prevented = true; } }, state, container);
  assert.equal(prevented, true);
  assert.equal(container.doc.activeElement, brand, 'Forward Tab from last element must wrap to first');

  // Shift+Tab backward from first element wraps to last
  prevented = false;
  simulateTrapKeyDown({ key: 'Tab', shiftKey: true, preventDefault: () => { prevented = true; } }, state, container);
  assert.equal(prevented, true);
  assert.equal(container.doc.activeElement, themeToggle, 'Shift+Tab from first element must wrap to last');
});

test('[Behavioral] Escape key closes open dropdown first before closing mobile drawer', () => {
  const brand = mockElement('a', { href: './' });
  const themeToggle = mockElement('button', { 'data-bs-toggle': 'dropdown', 'aria-expanded': 'true' });
  const dropdownMenu = mockElement('div', { classes: ['dropdown-menu', 'show'] });

  let toggleClicked = false;
  themeToggle.onclick = () => {
    toggleClicked = true;
    themeToggle.setAttribute('aria-expanded', 'false');
    dropdownMenu.classList.remove('show');
  };

  const container = mockContainer([brand, themeToggle, dropdownMenu]);
  const state = { mobileOpen: true };

  // Press Escape when dropdown is open inside sidebar
  simulateTrapKeyDown({ key: 'Escape' }, state, container);

  // Assert: Dropdown closed via toggle click, but mobile drawer stayed open!
  assert.equal(toggleClicked, true, 'Toggle click should be triggered to dismiss dropdown');
  assert.equal(state.mobileOpen, true, 'Mobile drawer MUST stay open on first Escape dismissal of dropdown');
  assert.equal(container.doc.activeElement, themeToggle, 'Focus must return to toggle button');

  // Press Escape a second time when no dropdown is open
  simulateTrapKeyDown({ key: 'Escape' }, state, container);
  assert.equal(state.mobileOpen, false, 'Second Escape press must close the mobile drawer');
});
