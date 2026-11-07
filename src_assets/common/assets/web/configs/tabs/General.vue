<script setup>
import Checkbox from '../../Checkbox.vue'
import { ref, onMounted, getCurrentInstance } from 'vue'
import {
  Play,
  Plus,
  Shield,
  Trash2,
  Undo,
} from '@lucide/vue'
import { apiFetch } from '../../fetch_utils'

const props = defineProps({
  platform: String,
  config: Object
})
const config = ref(props.config)
const instance = getCurrentInstance()

const sessionLayout = ref('concurrent')
const sessionLayoutStatus = ref('')
const sessionLayoutBusy = ref(false)
const sessionLayoutSuccess = ref(false)
const sessionLayoutError = ref(null)

async function fetchSessionLayout() {
  try {
    const response = await apiFetch('./api/session-layout', { method: 'GET' })
    const data = await response.json()
    if (data.layout) {
      sessionLayout.value = data.layout
      if (config.value) {
        config.value.session_layout = data.layout
      }
    }
    sessionLayoutStatus.value = data.status || ''
  } catch (e) {
    sessionLayoutStatus.value = ''
  }
}

async function applySessionLayout() {
  const t = instance?.proxy?.$t
  const confirmMsg = typeof t === 'function'
    ? t('config.session_layout_apply_confirm')
    : 'Applying a session layout may log out the current desktop session. Continue?'
  sessionLayoutBusy.value = true
  sessionLayoutSuccess.value = false
  sessionLayoutError.value = null
  if (!window.confirm(confirmMsg)) {
    sessionLayoutBusy.value = false
    return
  }
  try {
    const response = await apiFetch('./api/session-layout', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ layout: sessionLayout.value })
    })
    const data = await response.json().catch(() => ({}))
    if (response.ok && data.status) {
      sessionLayoutSuccess.value = true
      await fetchSessionLayout()
      setTimeout(() => { sessionLayoutSuccess.value = false }, 5000)
    } else {
      sessionLayoutError.value = data.error || 'Failed to apply session layout'
      setTimeout(() => { sessionLayoutError.value = null }, 5000)
    }
  } catch (e) {
    sessionLayoutError.value = 'Failed to apply session layout'
    setTimeout(() => { sessionLayoutError.value = null }, 5000)
  } finally {
    sessionLayoutBusy.value = false
  }
}

onMounted(() => {
  fetchSessionLayout()
})

function addCmd() {
  let template = {
    do: "",
    undo: "",
  };

  if (props.platform === 'windows') {
    template = { ...template, elevated: false };
  }
  config.value.global_prep_cmd.push(template);
}

function removeCmd(index) {
  config.value.global_prep_cmd.splice(index,1)
}
</script>

<template>
  <div id="general" class="config-page">
    <!-- Locale -->
    <div class="mb-3">
      <label for="locale" class="form-label">{{ $t('config.locale') }}</label>
      <select id="locale" class="form-select" v-model="config.locale">
        <option value="bg">Български (Bulgarian)</option>
        <option value="cs">Čeština (Czech)</option>
        <option value="de">Deutsch (German)</option>
        <option value="en">English</option>
        <option value="en_GB">English, United Kingdom</option>
        <option value="en_US">English, United States</option>
        <option value="es">Español (Spanish)</option>
        <option value="fr">Français (French)</option>
        <option value="hu">Magyar (Hungarian)</option>
        <option value="it">Italiano (Italian)</option>
        <option value="ja">日本語 (Japanese)</option>
        <option value="ko">한국어 (Korean)</option>
        <option value="pl">Polski (Polish)</option>
        <option value="pt">Português (Portuguese)</option>
        <option value="pt_BR">Português, Brasileiro (Portuguese, Brazilian)</option>
        <option value="ru">Русский (Russian)</option>
        <option value="sv">svenska (Swedish)</option>
        <option value="tr">Türkçe (Turkish)</option>
        <option value="uk">Українська (Ukranian)</option>
        <option value="vi">Tiếng Việt (Vietnamese)</option>
        <option value="zh">简体中文 (Chinese Simplified)</option>
        <option value="zh_TW">繁體中文 (Chinese Traditional)</option>
      </select>
      <div class="form-text">{{ $t('config.locale_desc') }}</div>
    </div>

    <!-- Sunshine Name -->
    <div class="mb-3">
      <label for="sunshine_name" class="form-label">{{ $t('config.sunshine_name') }}</label>
      <input type="text" class="form-control" id="sunshine_name" placeholder="Sunshine"
             v-model="config.sunshine_name" />
      <div class="form-text">{{ $t('config.sunshine_name_desc') }}</div>
    </div>

    <!-- Log Level -->
    <div class="mb-3">
      <label for="min_log_level" class="form-label">{{ $t('config.min_log_level') }}</label>
      <select id="min_log_level" class="form-select" v-model="config.min_log_level">
        <option value="0">{{ $t('config.min_log_level_0') }}</option>
        <option value="1">{{ $t('config.min_log_level_1') }}</option>
        <option value="2">{{ $t('config.min_log_level_2') }}</option>
        <option value="3">{{ $t('config.min_log_level_3') }}</option>
        <option value="4">{{ $t('config.min_log_level_4') }}</option>
        <option value="5">{{ $t('config.min_log_level_5') }}</option>
        <option value="6">{{ $t('config.min_log_level_6') }}</option>
      </select>
      <div class="form-text">{{ $t('config.min_log_level_desc') }}</div>
    </div>

    <!-- Session Layout -->
    <div class="mb-3">
      <label class="form-label">{{ $t('config.session_layout') }}</label>
      <div class="form-text">
        {{ $t('config.session_layout_current') }}: <strong>{{ sessionLayout }}</strong>
      </div>
      <div class="form-text" v-if="sessionLayoutStatus">
        {{ $t('config.session_layout_status') }}: {{ sessionLayoutStatus }}
      </div>
      <div class="form-check">
        <input type="radio" class="form-check-input" id="session_layout_concurrent" value="concurrent" v-model="sessionLayout" />
        <label class="form-check-label" for="session_layout_concurrent">
          {{ $t('config.session_layout_concurrent') }} — {{ $t('config.session_layout_concurrent_desc') }}
        </label>
      </div>
      <div class="form-check">
        <input type="radio" class="form-check-input" id="session_layout_switch" value="switch" v-model="sessionLayout" />
        <label class="form-check-label" for="session_layout_switch">
          {{ $t('config.session_layout_switch') }} — {{ $t('config.session_layout_switch_desc') }}
        </label>
      </div>
      <label for="session_layout_cmd" class="form-label mt-2">{{ $t('config.session_layout_cmd') }}</label>
      <input type="text" class="form-control" id="session_layout_cmd" v-model="config.session_layout_cmd" />
      <div class="form-text">{{ $t('config.session_layout_cmd_desc') }}</div>
      <button class="btn btn-primary mt-2" @click="applySessionLayout" :disabled="sessionLayoutBusy">
        {{ $t('config.session_layout_apply') }}
      </button>
      <div class="alert alert-success mt-2" v-if="sessionLayoutSuccess">{{ $t('config.session_layout_apply_success') }}</div>
      <div class="alert alert-danger mt-2" v-if="sessionLayoutError">{{ sessionLayoutError }}</div>
    </div>

    <!-- Global Prep Commands -->
    <div id="global_prep_cmd" class="mb-3 d-flex flex-column">
      <label class="form-label">{{ $t('config.global_prep_cmd') }}</label>
      <div class="form-text">{{ $t('config.global_prep_cmd_desc') }}</div>
      <table class="table" v-if="config.global_prep_cmd.length > 0">
        <thead>
        <tr>
          <th scope="col"><Play :size="16" /> {{ $t('_common.do_cmd') }}</th>
          <th scope="col"><Undo :size="16" /> {{ $t('_common.undo_cmd') }}</th>
          <th scope="col" v-if="platform === 'windows'">
            <Shield :size="16" /> {{ $t('_common.run_as') }}
          </th>
          <th scope="col"></th>
        </tr>
        </thead>
        <tbody>
        <tr v-for="(c, i) in config.global_prep_cmd">
          <td>
            <input type="text" class="form-control monospace" v-model="c.do" />
          </td>
          <td>
            <input type="text" class="form-control monospace" v-model="c.undo" />
          </td>
          <td v-if="platform === 'windows'" class="align-middle">
            <Checkbox :id="'prep-cmd-admin-' + i"
                      label="_common.elevated"
                      desc=""
                      v-model="c.elevated"
            ></Checkbox>
          </td>
          <td>
            <button class="btn btn-danger" @click="removeCmd(i)">
              <Trash2 :size="16" />
            </button>
            <button class="btn btn-success" @click="addCmd">
              <Plus :size="16" />
            </button>
          </td>
        </tr>
        </tbody>
      </table>
      <button class="ms-0 mt-2 btn btn-success" style="margin: 0 auto" @click="addCmd">
        &plus; {{ $t('config.add') }}
      </button>
    </div>

    <!-- Notify Pre-Releases -->
    <Checkbox class="mb-3"
              id="notify_pre_releases"
              locale-prefix="config"
              v-model="config.notify_pre_releases"
              default="false"
    ></Checkbox>

    <!-- Enable system tray -->
    <Checkbox class="mb-3"
              id="system_tray"
              locale-prefix="config"
              v-model="config.system_tray"
              default="true"
    ></Checkbox>
  </div>
</template>

<style scoped>

</style>
