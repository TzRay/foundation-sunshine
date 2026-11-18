<script setup>
import { computed, onBeforeUnmount, onMounted } from 'vue'
import Checkbox from '../../../components/Checkbox.vue'
import { matLiveFailureStage, useMatLiveStatus } from '../../../composables/useMatLiveStatus.js'

defineProps(['config'])
const { status, loading, start, dispose } = useMatLiveStatus()
const knownStates = new Set(['disabled', 'audio_disabled', 'unobserved', 'ready', 'streaming', 'idle', 'error', 'unsupported', 'starting', 'waiting_host', 'stopped'])
const state = computed(() => knownStates.has(status.value?.state) ? status.value.state : 'unavailable')
const source = computed(() => ['bed', 'native', 'objects'].includes(status.value?.source) ? status.value.source : '')
const failureStage = computed(() => matLiveFailureStage(status.value))
onMounted(start)
onBeforeUnmount(dispose)
</script>

<template>
  <div class="mb-3 border rounded p-3">
    <Checkbox id="mat_live" v-model="config.mat_live" true-value="enabled" false-value="disabled" />
    <div v-if="config.mat_live === 'enabled'" class="mt-2">
      <label for="mat_source" class="form-label">{{ $t('config.mat_source') }}</label>
      <select id="mat_source" v-model="config.mat_source" class="form-select">
        <option value="bed">{{ $t('config.mat_source_bed') }}</option>
        <option value="native">{{ $t('config.mat_source_native') }}</option>
        <option value="objects">{{ $t('config.mat_source_objects') }}</option>
      </select>
      <div class="form-text">{{ $t('config.mat_source_desc') }}</div>
      <div v-if="config.mat_source === 'native'" class="form-text">{{ $t('config.mat_source_native_desc') }}</div>
    </div>
    <div class="small mt-2" role="status" aria-live="polite">
      {{ $t('config.mat_live_status') }}: {{ $t(loading && !status ? 'config.mat_live_loading' : `config.mat_live_state_${state}`) }}
    </div>
    <div v-if="source" class="small mt-1">{{ $t('config.mat_live_capture_source') }}: {{ $t('config.mat_source_' + source) }}</div>
    <div v-if="status?.endpoint_confirmed && status?.endpoint" class="small mt-1 text-break">{{ $t('config.mat_live_endpoint') }}: {{ status.endpoint }}</div>
    <div v-if="status?.negotiated_codec === 'mat-live-v1'" class="small mt-1">{{ $t('config.mat_live_codec') }}: MAT</div>
    <div v-if="status?.negotiated_codec === 'mat-live-v1'" class="form-text">{{ $t('config.mat_live_output_boundary') }}</div>
    <div v-if="status?.first_error" class="small text-danger mt-1">
      {{ $t('config.mat_live_first_error') }} ({{ $t('config.mat_live_stage_' + failureStage) }}): {{ status.first_error }}
    </div>
    <div v-else-if="status?.last_error" class="small text-danger mt-1">{{ status.last_error }}</div>
  </div>
</template>
