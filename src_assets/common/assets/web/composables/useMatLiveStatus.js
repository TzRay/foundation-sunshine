import { ref } from 'vue'
import { apiJson } from '../utils/apiFetch.js'

export function matLiveFailureStage(status) {
  const stages = {
    prepare: 'source_prepare',
    'source-read': 'source_delivery',
    'encrypt-send': 'send',
    'send-queue': 'send_queue',
    pipe_prepare: 'source_prepare',
    pipe_play: 'source_start',
    pipe_owner: 'source_process',
    pipe_header: 'source_delivery',
    pipe_payload: 'source_delivery',
    pipe_end_ack: 'source_delivery',
    pipe_restart: 'audio_recovery',
    audio_recovery: 'audio_recovery',
    pacing: 'send_schedule',
    credit: 'send_queue',
    send_queue: 'send_queue',
    send: 'send',
    sender: 'send',
  }
  return stages[status?.first_error_stage] ?? 'unknown'
}

export function useMatLiveStatus() {
  const status = ref(null)
  const loading = ref(false)
  let active = true
  let timer
  const refresh = async () => {
    if (!active || loading.value) return
    loading.value = true
    try {
      const next = await apiJson('/api/mat/status', { cache: 'no-store', signal: AbortSignal.timeout(2500) })
      if (active) status.value = next
    } catch {
      if (active) status.value = { state: 'unavailable', last_error: '' }
    } finally {
      loading.value = false
    }
  }
  const start = () => {
    if (!active || timer !== undefined) return
    const tick = () => { if (!document.hidden) void refresh() }
    tick()
    timer = setInterval(tick, 3000)
  }
  const dispose = () => {
    active = false
    clearInterval(timer)
  }
  return { status, loading, refresh, start, dispose }
}
