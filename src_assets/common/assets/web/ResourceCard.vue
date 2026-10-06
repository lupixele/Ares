<template>
  <div class="card my-4 ares-card">
    <div class="card-body">
      <h2 class="h5 mb-2">{{ $t('resource_card.resources') }}</h2>
      <div class="d-flex flex-wrap gap-2 mt-3">
        <a class="btn btn-outline-success btn-sm d-inline-flex align-items-center gap-1" href="https://app.lizardbyte.dev" target="_blank" rel="noopener noreferrer">
          <Globe :size="16" class="icon"></Globe>
          {{ $t('resource_card.lizardbyte_website') }}
        </a>
        <a class="btn btn-outline-info btn-sm d-inline-flex align-items-center gap-1" :href="documentationUrl" target="_blank" rel="noopener noreferrer">
          <BookOpen :size="16" class="icon"></BookOpen>
          {{ $t('resource_card.documentation') }}
        </a>
        <a class="btn btn-outline-primary btn-sm d-inline-flex align-items-center gap-1" href="https://app.lizardbyte.dev/discord" target="_blank" rel="noopener noreferrer">
          <SimpleIcon icon="Discord" :size="16" class="icon"></SimpleIcon>
          Discord
        </a>
        <a class="btn btn-outline-secondary btn-sm d-inline-flex align-items-center gap-1" href="https://github.com/orgs/LizardByte/discussions" target="_blank" rel="noopener noreferrer">
          <SimpleIcon icon="GitHub" :size="16" class="icon"></SimpleIcon>
          {{ $t('resource_card.github_discussions') }}
        </a>
      </div>
    </div>
  </div>

  <!-- Legal & Lineage -->
  <div class="card my-4 ares-card">
    <div class="card-body">
      <h2 class="h5 mb-2">{{ $t('resource_card.legal') }}</h2>
      <p class="text-muted small mb-3">{{ $t('resource_card.legal_desc') }}</p>
      <div class="d-flex flex-wrap gap-2 mb-3">
        <a class="btn btn-outline-danger btn-sm d-inline-flex align-items-center gap-1" href="https://github.com/LizardByte/Sunshine/blob/master/LICENSE"
          target="_blank" rel="noopener noreferrer">
          <FileText :size="16" class="icon"></FileText>
          {{ $t('resource_card.license') }}
        </a>
        <a class="btn btn-outline-danger btn-sm d-inline-flex align-items-center gap-1" href="https://github.com/ClassicOldSong/Apollo/blob/master/LICENSE"
          target="_blank" rel="noopener noreferrer">
          <FileText :size="16" class="icon"></FileText>
          Apollo License
        </a>
        <a class="btn btn-outline-secondary btn-sm d-inline-flex align-items-center gap-1" href="https://github.com/LizardByte/Sunshine/blob/master/THIRDPARTY.md"
          target="_blank" rel="noopener noreferrer">
          <FileText :size="16" class="icon"></FileText>
          {{ $t('resource_card.third_party_notice') }}
        </a>
      </div>
      <div class="small text-muted border-top pt-2">
        <em>{{ $t('resource_card.upstream_credits_desc') }}</em>
      </div>
    </div>
  </div>
</template>

<script>
  import {
    BookOpen,
    FileText,
    Globe
  } from '@lucide/vue'
  import SimpleIcon from './SimpleIcon.vue'

  export default {
    name: 'ResourceCard',
    components: {
      BookOpen,
      FileText,
      Globe,
      SimpleIcon
    },
    props: {
      installedVersionNotStable: Boolean
    },
    computed: {
      documentationUrl() {
        if (this.installedVersionNotStable) {
          return "https://docs.lizardbyte.dev/projects/sunshine/en/latest"
        }
        return "https://docs.lizardbyte.dev/projects/sunshine"
      }
    }
  }
</script>

<style scoped>
.ares-card {
  border-radius: 12px;
  border: 1px solid var(--bs-border-color);
}
</style>
