<template>
  <Navbar></Navbar>
  <div id="content" class="container ares-password">
    <header class="ares-password__header my-4">
      <h1 class="ares-password__title">{{ $t('password.password_change') }}</h1>
      <p class="ares-password__desc text-muted mb-0">{{ $t('password.password_change_desc') }}</p>
    </header>

    <form @submit.prevent="save">
      <div class="row g-3">
        <div class="col-12 col-lg-6">
          <section class="card h-100 ares-password__card">
            <div class="card-body">
              <h2 class="ares-password__card-title">
                <LogIn :size="16" class="icon me-2"></LogIn>
                {{ $t('password.current_creds') }}
              </h2>

              <div class="mb-3">
                <label for="currentUsername" class="form-label">{{ $t('_common.username') }}</label>
                <input required type="text" class="form-control" id="currentUsername" autocomplete="username"
                  v-model="passwordData.currentUsername" />
              </div>

              <div class="mb-0">
                <label for="currentPassword" class="form-label">{{ $t('_common.password') }}</label>
                <input autocomplete="current-password" type="password" class="form-control" id="currentPassword"
                  v-model="passwordData.currentPassword" />
              </div>
            </div>
          </section>
        </div>

        <div class="col-12 col-lg-6">
          <section class="card h-100 ares-password__card">
            <div class="card-body">
              <h2 class="ares-password__card-title">
                <ShieldCheck :size="16" class="icon me-2"></ShieldCheck>
                {{ $t('password.new_creds') }}
              </h2>

              <div class="mb-3">
                <label for="newUsername" class="form-label">{{ $t('_common.username') }}</label>
                <input type="text" class="form-control" id="newUsername" autocomplete="username"
                  aria-describedby="newUsernameHelp" v-model="passwordData.newUsername" />
                <div id="newUsernameHelp" class="form-text">{{ $t('password.new_username_desc') }}</div>
              </div>

              <div class="mb-3">
                <label for="newPassword" class="form-label">{{ $t('_common.password') }}</label>
                <input autocomplete="new-password" required type="password" class="form-control" id="newPassword"
                  v-model="passwordData.newPassword" />
              </div>

              <div class="mb-0">
                <label for="confirmNewPassword" class="form-label">{{ $t('password.confirm_password') }}</label>
                <input autocomplete="new-password" required type="password" class="form-control" id="confirmNewPassword"
                  v-model="passwordData.confirmNewPassword" />
              </div>
            </div>
          </section>
        </div>
      </div>

      <div class="alert alert-danger my-3 d-flex align-items-start gap-2" v-if="error" role="alert">
        <AlertCircle :size="16" class="icon flex-shrink-0 mt-1"></AlertCircle>
        <div><strong>{{ $t('_common.error') }}</strong> {{ error }}</div>
      </div>

      <div class="alert alert-success my-3 d-flex align-items-start gap-2" v-if="success" role="status"
        aria-live="polite">
        <CheckCircle2 :size="16" class="icon flex-shrink-0 mt-1"></CheckCircle2>
        <div><strong>{{ $t('_common.success') }}</strong> {{ $t('password.success_msg') }}</div>
      </div>

      <div class="ares-password__actions">
        <button type="submit" class="btn btn-primary d-inline-flex align-items-center gap-2">
          <Save :size="18" class="icon"></Save>
          {{ $t('_common.save') }}
        </button>
      </div>
    </form>
  </div>
</template>

<script>
  import Navbar from './Navbar.vue'
  import { apiFetch } from './fetch_utils'
  import { AlertCircle, CheckCircle2, LogIn, Save, ShieldCheck } from '@lucide/vue'

  export default {
    components: {
      Navbar,
      AlertCircle,
      CheckCircle2,
      LogIn,
      Save,
      ShieldCheck,
    },
    data() {
      return {
        error: null,
        success: false,
        passwordData: {
          currentUsername: "",
          currentPassword: "",
          newUsername: "",
          newPassword: "",
          confirmNewPassword: "",
        },
      };
    },
    methods: {
      save() {
        this.error = null;
        apiFetch("./api/password", {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body: JSON.stringify(this.passwordData),
        }).then((r) => {
          if (r.status === 200) {
            r.json().then((rj) => {
              this.success = rj.status;
              if (this.success === true) {
                setTimeout(() => {
                  document.location.reload();
                }, 5000);
              } else {
                this.error = rj.error;
              }
            });
          } else {
            this.error = "Internal Server Error";
          }
        });
      },
    },
  }
</script>

<style scoped>
  .ares-password__header {
    border-bottom: 1px solid var(--bs-border-color);
    padding-bottom: 1rem;
  }

  .ares-password__title {
    font-size: 1.75rem;
    font-weight: 700;
    margin-bottom: 0.25rem;
  }

  .ares-password__desc {
    font-size: 0.95rem;
    max-width: 70ch;
  }

  .ares-password__card {
    border-radius: 12px;
    border: 1px solid var(--bs-border-color);
    background-color: var(--bs-body-bg);
  }

  .ares-password__card .card-body {
    padding: 1.25rem 1.5rem;
  }

  .ares-password__card-title {
    display: flex;
    align-items: center;
    font-size: 0.75rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.06em;
    color: var(--bs-secondary-color);
    margin-bottom: 1rem;
  }

  .ares-password__actions {
    display: flex;
    justify-content: flex-start;
    margin-top: 1.5rem;
  }
</style>
