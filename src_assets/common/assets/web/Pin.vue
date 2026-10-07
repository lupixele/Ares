<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <h1 class="my-4 text-center">{{ $t('pin.pin_pairing') }}</h1>
    <form class="form d-flex flex-column align-items-center" id="form" @submit.prevent="registerDevice">
      <div class="card flex-column d-flex p-4 mb-4">
        <div class="input-group mt-2">
          <label for="pairing-input" class="visually-hidden">{{ $t('pin.select_pairing') }}</label>
          <span class="input-group-text">
            <user-round-search :size="18" class="icon"></user-round-search>
          </span>
          <select v-model="selectedPairingId" class="form-select" id="pairing-input" required>
            <option disabled value="">
              {{ pendingPairings.length ? $t('pin.select_pairing') : $t('pin.no_pending_pairings') }}
            </option>
            <option v-for="pairing in pendingPairings" :key="pairing.id" :value="pairing.id">
              {{ pairing.name || $t('pin.unknown_device') }} — {{ pairing.address || $t('pin.unknown_address') }}
            </option>
          </select>
          <button
            type="button"
            class="btn btn-outline-danger"
            :disabled="!selectedPairingId"
            :title="$t('pin.cancel_pairing')"
            @click="cancelSelectedPairing"
          >
            <x :size="18" class="icon"></x>
          </button>
        </div>
        <div class="input-group mt-2">
          <label for="pin-input" class="visually-hidden">{{ $t('navbar.pin') }}</label>
          <span class="input-group-text">
            <hash :size="18" class="icon"></hash>
          </span>
          <input v-model="pin" type="text" pattern="\d{4}" maxlength="4" inputmode="numeric"
            class="form-control" id="pin-input" :placeholder="$t('navbar.pin')" required />
        </div>
        <div class="input-group mt-2">
          <label for="name-input" class="visually-hidden">{{ $t('pin.device_name') }}</label>
          <span class="input-group-text">
            <monitor :size="18" class="icon"></monitor>
          </span>
          <input v-model="name" type="text" class="form-control" id="name-input"
            :placeholder="`${$t('pin.device_name')} (${$t('_common.optional')})`" />
        </div>
        <button type="submit" class="btn btn-primary mt-4">
          <forward :size="18" class="icon"></forward>
          {{ $t('pin.send') }}
        </button>
      </div>
      <div class="alert alert-warning" role="alert">
        <b>{{ $t('_common.warning') }}</b> {{ $t('pin.warning_msg') }}
      </div>
      <div v-if="status" :class="`alert alert-${status.type}`" role="alert">{{ status.message }}</div>
    </form>

    <hr class="my-5" />

    <!-- Authenticated Paired Clients Administration Section -->
    <section id="paired-clients-section" class="w-100 my-4" aria-labelledby="paired-clients-heading">
      <div class="d-flex justify-content-between align-items-center mb-4">
        <div>
          <h2 id="paired-clients-heading" class="h3 mb-1">{{ $t('pin.paired_clients_title') }}</h2>
        </div>
        <button
          type="button"
          class="btn btn-outline-secondary btn-sm d-inline-flex align-items-center gap-1"
          :disabled="clientsLoading"
          @click="loadClients"
        >
          <refresh-cw :size="16" :class="{ 'spin': clientsLoading }"></refresh-cw>
          {{ $t('pin.refresh_clients') }}
        </button>
      </div>

      <div v-if="clientsLoading && !clients.length" class="text-center py-4">
        <div class="spinner-border text-primary" role="status">
          <span class="visually-hidden">{{ $t('_common.loading') }}</span>
        </div>
      </div>

      <div v-else-if="clientsError" class="alert alert-danger" role="alert">
        {{ clientsError }}
      </div>

      <div v-else-if="!clients.length" class="alert alert-secondary text-center" role="alert">
        {{ $t('pin.no_paired_clients') }}
      </div>

      <div v-else class="d-flex flex-column gap-4">
        <div
          v-for="client in clients"
          :key="client.uuid"
          class="card shadow-sm"
          :data-client-uuid="client.uuid"
        >
          <div class="card-header d-flex flex-wrap justify-content-between align-items-center gap-2">
            <div>
              <h3 class="h5 mb-1 d-inline-block me-2">{{ client.name || $t('pin.unnamed_client') }}</h3>
              <span
                class="badge"
                :class="client.draft.enabled ? 'bg-success' : 'bg-secondary'"
              >
                {{ client.draft.enabled ? $t('_common.enabled') : $t('_common.disabled') }}
              </span>
              <span v-if="isDirty(client)" class="badge bg-warning text-dark ms-2">
                {{ $t('pin.unsaved_changes') }}
              </span>
            </div>
            <div class="text-muted small font-monospace text-break">
              {{ client.uuid }}
            </div>
          </div>

          <div class="card-body">
            <!-- Client Enabled Switch -->
            <div class="form-check form-switch mb-3">
              <input
                class="form-check-input"
                type="checkbox"
                role="switch"
                :id="'client-enabled-' + client.uuid"
                v-model="client.draft.enabled"
              />
              <label class="form-check-label fw-semibold" :for="'client-enabled-' + client.uuid">
                {{ $t('pin.client_enabled_label') }}
              </label>
            </div>

            <!-- Permission Groups Grid -->
            <div class="row g-4">
              <!-- Input Permissions -->
              <div class="col-12 col-md-6">
                <div class="border rounded p-3 h-100 bg-body-tertiary">
                  <h4 class="h6 text-uppercase fw-bold text-muted mb-3 d-flex align-items-center gap-2">
                    <gamepad-2 :size="16"></gamepad-2>
                    {{ $t('pin.input_permissions') }}
                  </h4>
                  <div class="d-flex flex-column gap-2">
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-ctrl-' + client.uuid"
                        v-model="client.draft.controller"
                      />
                      <label class="form-check-label" :for="'perm-ctrl-' + client.uuid">
                        {{ $t('pin.input_controller') }}
                      </label>
                    </div>
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-touch-' + client.uuid"
                        v-model="client.draft.touch"
                      />
                      <label class="form-check-label" :for="'perm-touch-' + client.uuid">
                        {{ $t('pin.input_touch') }}
                      </label>
                    </div>
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-pen-' + client.uuid"
                        v-model="client.draft.pen"
                      />
                      <label class="form-check-label" :for="'perm-pen-' + client.uuid">
                        {{ $t('pin.input_pen') }}
                      </label>
                    </div>
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-mouse-' + client.uuid"
                        v-model="client.draft.mouse"
                      />
                      <label class="form-check-label" :for="'perm-mouse-' + client.uuid">
                        {{ $t('pin.input_mouse') }}
                      </label>
                    </div>
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-kbd-' + client.uuid"
                        v-model="client.draft.keyboard"
                      />
                      <label class="form-check-label" :for="'perm-kbd-' + client.uuid">
                        {{ $t('pin.input_kbd') }}
                      </label>
                    </div>
                  </div>
                </div>
              </div>

              <!-- Action Permissions -->
              <div class="col-12 col-md-6">
                <div class="border rounded p-3 h-100 bg-body-tertiary">
                  <h4 class="h6 text-uppercase fw-bold text-muted mb-3 d-flex align-items-center gap-2">
                    <shield :size="16"></shield>
                    {{ $t('pin.action_permissions') }}
                  </h4>
                  <div class="d-flex flex-column gap-2">
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-view-' + client.uuid"
                        v-model="client.draft.view"
                      />
                      <label class="form-check-label" :for="'perm-view-' + client.uuid">
                        {{ $t('pin.perm_view') }}
                      </label>
                    </div>
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-list-' + client.uuid"
                        v-model="client.draft.list"
                      />
                      <label class="form-check-label" :for="'perm-list-' + client.uuid">
                        {{ $t('pin.perm_list') }}
                      </label>
                    </div>
                    <div class="form-check">
                      <input
                        class="form-check-input"
                        type="checkbox"
                        :id="'perm-launch-' + client.uuid"
                        v-model="client.draft.launch"
                      />
                      <label class="form-check-label" :for="'perm-launch-' + client.uuid">
                        {{ $t('pin.perm_launch') }}
                      </label>
                    </div>
                  </div>
                </div>
              </div>
            </div>

            <!-- Active Stream Disconnect Warning -->
            <div
              v-if="willDisconnectStreams(client)"
              class="alert alert-warning d-flex align-items-center gap-2 mt-3 mb-0"
              role="alert"
            >
              <alert-triangle :size="18" class="flex-shrink-0 text-warning-emphasis"></alert-triangle>
              <div class="small">
                {{ $t('pin.stream_disconnect_warning') }}
              </div>
            </div>

            <!-- Inline Confirmation for Disabling or Permission Reduction -->
            <div
              v-if="client.confirmingSave"
              class="alert alert-danger mt-3 mb-0"
              role="alert"
              aria-live="assertive"
            >
              <h5 class="h6 fw-bold mb-1">{{ $t('pin.confirm_reduction_title') }}</h5>
              <p class="small mb-3">{{ $t('pin.confirm_reduction_msg') }}</p>
              <div class="d-flex gap-2">
                <button
                  type="button"
                  class="btn btn-sm btn-danger d-inline-flex align-items-center gap-1 btn-confirm-save"
                  :disabled="client.saving"
                  @click="confirmSave(client)"
                >
                  <check :size="16"></check>
                  {{ $t('pin.confirm_save') }}
                </button>
                <button
                  type="button"
                  class="btn btn-sm btn-secondary d-inline-flex align-items-center gap-1 btn-cancel-confirm"
                  :disabled="client.saving"
                  @click="cancelConfirm(client)"
                >
                  <x :size="16"></x>
                  {{ $t('_common.cancel') }}
                </button>
              </div>
            </div>

            <!-- Error and Success Feedback -->
            <div v-if="client.error" class="alert alert-danger mt-3 mb-0" role="alert">
              {{ client.error }}
            </div>
            <div v-if="client.successMessage" class="alert alert-success mt-3 mb-0" role="alert">
              {{ client.successMessage }}
            </div>
          </div>

          <div class="card-footer d-flex flex-wrap align-items-center gap-2 bg-body">
            <button
              type="button"
              class="btn btn-primary d-inline-flex align-items-center gap-1 btn-save-client"
              :disabled="!isDirty(client) || client.saving"
              @click="handleSaveClick(client)"
            >
              <span v-if="client.saving" class="spinner-border spinner-border-sm" role="status" aria-hidden="true"></span>
              <save v-else :size="16" class="icon"></save>
              {{ client.saving ? $t('_common.loading') : $t('_common.save') }}
            </button>

            <button
              type="button"
              class="btn btn-outline-secondary d-inline-flex align-items-center gap-1 btn-reset-draft"
              :disabled="!isDirty(client) || client.saving"
              @click="resetDraft(client)"
            >
              <rotate-ccw :size="16" class="icon"></rotate-ccw>
              {{ $t('pin.reset_draft') }}
            </button>

            <button
              type="button"
              class="btn btn-outline-danger btn-sm ms-auto d-inline-flex align-items-center gap-1 btn-unpair-client"
              :disabled="client.saving"
              :title="$t('pin.unpair_client')"
              @click="unpairClient(client)"
            >
              <trash-2 :size="16" class="icon"></trash-2>
              {{ $t('pin.unpair_client') }}
            </button>
          </div>
        </div>
      </div>
    </section>
  </div>
</template>

<script>
  import Navbar from './Navbar.vue'
  import { apiFetch } from './fetch_utils'
  import {
    PERM_BITS,
    EDITABLE_PERM_MASK,
    permToFlags,
    flagsToMask,
    computeOutgoingPerm,
    isReductionOrDisable,
  } from './client_permissions'
  import {
    AlertTriangle,
    Check,
    Forward,
    Gamepad2,
    Hash,
    Monitor,
    RefreshCw,
    RotateCcw,
    Save,
    Shield,
    Trash2,
    UserRoundSearch,
    X,
  } from '@lucide/vue'

  export {
    PERM_BITS,
    EDITABLE_PERM_MASK,
    permToFlags,
    flagsToMask,
    computeOutgoingPerm,
    isReductionOrDisable,
  }

  export default {
    components: {
      Navbar,
      AlertTriangle,
      Check,
      Forward,
      Gamepad2,
      Hash,
      Monitor,
      RefreshCw,
      RotateCcw,
      Save,
      Shield,
      Trash2,
      UserRoundSearch,
      X,
    },
    inject: {
      i18n: {
        default: () => ({ t: (k) => k }),
      },
    },
    data() {
      return {
        name: '',
        pendingPairings: [],
        pin: '',
        refreshTimer: null,
        selectedPairingId: '',
        status: null,
        // Paired clients state
        clients: [],
        clientsLoading: false,
        clientsError: null,
      };
    },
    mounted() {
      this.loadPendingPairings();
      this.loadClients();
      this.refreshTimer = window.setInterval(() => this.loadPendingPairings(), 2000);
    },
    beforeUnmount() {
      window.clearInterval(this.refreshTimer);
    },
    methods: {
      /**
       * Refresh the authenticated list of pairing requests awaiting approval.
       */
      async loadPendingPairings() {
        try {
          const response = await apiFetch('./api/pin', {method: 'GET'});
          if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
          }

          const body = await response.json();
          this.pendingPairings = body.pairings || [];
          if (!this.pendingPairings.some((pairing) => pairing.id === this.selectedPairingId)) {
            this.selectedPairingId = this.pendingPairings.length === 1 ? this.pendingPairings[0].id : '';
          }
        } catch (error) {
          console.error('Failed to load pending pairing requests', error);
          this.status = {type: 'danger', message: this.i18n.t('pin.load_failure')};
        }
      },

      /**
       * Apply the entered PIN only to the pairing request selected by the operator.
       */
      async registerDevice() {
        this.status = null;
        if (!this.selectedPairingId) {
          this.status = {type: 'danger', message: this.i18n.t('pin.select_pairing_required')};
          return;
        }

        const body = JSON.stringify({
          pairing_id: this.selectedPairingId,
          pin: this.pin,
          name: this.name,
        });
        const response = await apiFetch('./api/pin', {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body,
        });
        const result = await response.json();
        if (result.status === true) {
          this.status = {type: 'success', message: this.i18n.t('pin.pair_success')};
          this.pin = '';
          this.name = '';
        } else {
          this.status = {type: 'danger', message: this.i18n.t('pin.pair_failure')};
        }
        await this.loadPendingPairings();
        await this.loadClients();
      },

      /**
       * Cancel the selected pending request without affecting other clients.
       */
      async cancelSelectedPairing() {
        if (!this.selectedPairingId) {
          return;
        }

        const response = await apiFetch('./api/pin', {
          method: "DELETE",
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify({
            pairing_id: this.selectedPairingId,
          }),
        });
        const result = await response.json();
        if (result.status === true) {
          this.status = {type: 'success', message: this.i18n.t('pin.cancel_success')};
        } else {
          this.status = {type: 'danger', message: this.i18n.t('pin.cancel_failure')};
        }
        await this.loadPendingPairings();
      },

      /**
       * Load authenticated list of paired clients.
       */
      async loadClients() {
        this.clientsLoading = true;
        this.clientsError = null;
        try {
          const response = await apiFetch('./api/clients/list', { method: 'GET' });
          if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
          }
          const data = await response.json();
          if (data && data.status !== false && Array.isArray(data.named_certs)) {
            this.syncClientsList(data.named_certs);
          } else {
            throw new Error(data?.error || this.i18n.t('pin.load_clients_failed'));
          }
        } catch (err) {
          console.error('Failed to load paired clients', err);
          this.clientsError = err.message || this.i18n.t('pin.load_clients_failed');
        } finally {
          this.clientsLoading = false;
        }
      },

      /**
       * Merge newly loaded clients into view model while preserving dirty drafts.
       *
       * @param {Array} namedCerts - Array of client records from server.
       */
      syncClientsList(namedCerts) {
        const existingMap = new Map(this.clients.map(c => [c.uuid, c]));
        const updated = [];

        for (const cert of namedCerts) {
          const serverEnabled = Boolean(cert.enabled);
          const serverPerm = Number(cert.perm) >>> 0;
          const serverFlags = permToFlags(serverPerm);
          const existing = existingMap.get(cert.uuid);

          if (existing) {
            existing.name = cert.name;
            // Preserve operator draft and baseline if client is currently dirty or saving
            if (!this.isDirty(existing) && !existing.saving) {
              existing.persisted = {
                enabled: serverEnabled,
                perm: serverPerm,
                flags: serverFlags,
              };
              existing.draft = {
                enabled: serverEnabled,
                ...serverFlags,
              };
            }
            updated.push(existing);
          } else {
            updated.push({
              uuid: cert.uuid,
              name: cert.name,
              persisted: {
                enabled: serverEnabled,
                perm: serverPerm,
                flags: serverFlags,
              },
              draft: {
                enabled: serverEnabled,
                ...serverFlags,
              },
              saving: false,
              confirmingSave: false,
              error: null,
              successMessage: null,
            });
          }
        }

        this.clients = updated;
      },

      /**
       * Check if client draft differs from its acknowledged server baseline.
       *
       * @param {object} client - Client model.
       * @returns {boolean} True if draft has uncommitted changes.
       */
      isDirty(client) {
        if (!client || !client.draft || !client.persisted) return false;
        if (client.draft.enabled !== client.persisted.enabled) return true;
        const pFlags = client.persisted.flags;
        return (
          client.draft.controller !== pFlags.controller ||
          client.draft.touch !== pFlags.touch ||
          client.draft.pen !== pFlags.pen ||
          client.draft.mouse !== pFlags.mouse ||
          client.draft.keyboard !== pFlags.keyboard ||
          client.draft.list !== pFlags.list ||
          client.draft.view !== pFlags.view ||
          client.draft.launch !== pFlags.launch
        );
      },

      /**
       * Check if draft would cause live streaming sessions to be disconnected.
       * Disabling a client or revoking view/launch or any input permission triggers teardown.
       *
       * @param {object} client - Client model.
       * @returns {boolean} True if server will terminate active streams upon update.
       */
      willDisconnectStreams(client) {
        if (!client || !client.draft || !client.persisted) return false;
        return isReductionOrDisable(
          { enabled: client.persisted.enabled, flags: client.persisted.flags },
          { enabled: client.draft.enabled, flags: client.draft }
        );
      },

      /**
       * Intercept save button click: if reduction or disable is requested, prompt for confirmation.
       *
       * @param {object} client - Client model.
       */
      handleSaveClick(client) {
        if (client.saving) return;
        if (this.willDisconnectStreams(client)) {
          client.confirmingSave = true;
        } else {
          this.saveClient(client);
        }
      },

      /**
       * Operator confirmed disconnection reduction or disable; proceed with save.
       *
       * @param {object} client - Client model.
       */
      confirmSave(client) {
        client.confirmingSave = false;
        this.saveClient(client);
      },

      /**
       * Operator cancelled confirmation prompt.
       *
       * @param {object} client - Client model.
       */
      cancelConfirm(client) {
        client.confirmingSave = false;
      },

      /**
       * Reset client draft back to current persisted baseline.
       *
       * @param {object} client - Client model.
       */
      resetDraft(client) {
        client.draft = {
          enabled: client.persisted.enabled,
          ...client.persisted.flags,
        };
        client.confirmingSave = false;
        client.error = null;
      },

      /**
       * Commit draft changes to backend via POST /api/clients/update.
       *
       * @param {object} client - Client model.
       */
      async saveClient(client) {
        if (client.saving) return;

        // Snapshot draft submitted at this exact moment
        const submittedDraft = { ...client.draft };
        const outgoingEnabled = submittedDraft.enabled;
        const outgoingPerm = computeOutgoingPerm(client.persisted.perm, submittedDraft);

        client.saving = true;
        client.error = null;
        client.successMessage = null;
        client.confirmingSave = false;

        try {
          // Fetch fresh CSRF token
          let csrfToken = '';
          try {
            const tokenRes = await apiFetch('./api/csrf-token', { method: 'GET' });
            if (tokenRes.ok) {
              const tokenData = await tokenRes.json();
              if (tokenData && tokenData.csrf_token) {
                csrfToken = tokenData.csrf_token;
              }
            }
          } catch (e) {
            console.debug('Failed to fetch CSRF token', e);
          }

          const headers = {
            'Content-Type': 'application/json',
          };
          if (csrfToken) {
            headers['X-CSRF-Token'] = csrfToken;
          }

          const payload = {
            uuid: client.uuid,
            enabled: outgoingEnabled,
            perm: outgoingPerm,
          };

          const response = await apiFetch('./api/clients/update', {
            method: 'POST',
            headers,
            body: JSON.stringify(payload),
          });

          if (!response.ok) {
            let errMsg = `HTTP ${response.status}`;
            try {
              const errJson = await response.json();
              if (errJson && errJson.error) {
                errMsg = errJson.error;
              }
            } catch (_) {}
            throw new Error(errMsg);
          }

          let result;
          try {
            result = await response.json();
          } catch (jsonErr) {
            throw new Error('Malformed server response');
          }

          if (result && result.status === false) {
            throw new Error(result.error || this.i18n.t('pin.client_update_failed'));
          }

          // Successfully persisted: update persisted baseline to what was submitted
          client.persisted.enabled = outgoingEnabled;
          client.persisted.perm = outgoingPerm;
          client.persisted.flags = permToFlags(outgoingPerm);

          client.successMessage = this.i18n.t('pin.client_updated_success');
          setTimeout(() => {
            if (client.successMessage) {
              client.successMessage = null;
            }
          }, 4000);

          await this.loadClients();

        } catch (err) {
          console.error('Failed to update client', err);
          client.error = err.message || this.i18n.t('pin.client_update_failed');
          // On failure, persisted is NOT updated, so draft remains dirty
        } finally {
          client.saving = false;
        }
      },

      /**
       * Unpair client via POST /api/clients/unpair.
       *
       * @param {object} client - Client model.
       */
      async unpairClient(client) {
        if (client.saving) return;
        client.saving = true;
        client.error = null;

        try {
          let csrfToken = '';
          try {
            const tokenRes = await apiFetch('./api/csrf-token', { method: 'GET' });
            if (tokenRes.ok) {
              const tokenData = await tokenRes.json();
              if (tokenData && tokenData.csrf_token) {
                csrfToken = tokenData.csrf_token;
              }
            }
          } catch (_) {}

          const headers = { 'Content-Type': 'application/json' };
          if (csrfToken) headers['X-CSRF-Token'] = csrfToken;

          const response = await apiFetch('./api/clients/unpair', {
            method: 'POST',
            headers,
            body: JSON.stringify({ uuid: client.uuid }),
          });

          if (!response.ok) {
            let errMsg = `HTTP ${response.status}`;
            try {
              const errJson = await response.json();
              if (errJson && errJson.error) errMsg = errJson.error;
            } catch (_) {}
            throw new Error(errMsg);
          }

          const result = await response.json();
          if (result && result.status === false) {
            throw new Error(result.error || this.i18n.t('pin.unpair_failed'));
          }

          await this.loadClients();
        } catch (err) {
          console.error('Failed to unpair client', err);
          client.error = err.message || this.i18n.t('pin.unpair_failed');
        } finally {
          client.saving = false;
        }
      },
    },
  };
</script>

<style scoped>
.spin {
  animation: spin 1s linear infinite;
}
@keyframes spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}
</style>
