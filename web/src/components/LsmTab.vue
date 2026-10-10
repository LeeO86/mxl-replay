<script setup>
// LSM / edit screen (SPECIFICATION.md §8.2): monitor and cameras on the left, transport, speed
// and modes on the right, the buffer timeline below.
import { computed, onUnmounted, ref, watch } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import CreateClipDialog from "./CreateClipDialog.vue";
import LivePicture from "./LivePicture.vue";
import Segmented from "./Segmented.vue";
import Timeline from "./Timeline.vue";
import { AUDIO_MODES, fmtNs, fmtSpeed, MOTION_MODES, SPEED_PRESETS } from "../api.js";
import { keyLabel, keymap } from "../keys.js";
import {
  active,
  cameraColour,
  cameraLabel,
  cameras,
  channelPost,
  channelState,
  clipsById,
  marks,
  scrubFrames,
  scrubSeconds,
  setAngle,
  setMode,
  setSpeed,
  state,
  transport,
} from "../store.js";

const creating = ref(false);
const canCreate = computed(() => active.value?.has_in && active.value?.has_out && active.value.out_ns > active.value.in_ns);
const markedNs = computed(() => (canCreate.value ? active.value.out_ns - active.value.in_ns : null));
const playingClip = computed(() => clipsById.value[active.value?.clip]);

// Speed fader: the channel's speed, or the fader's own value while it is dragged.
const reverse = ref(false);
const dragValue = ref(null);
// The speed the channel was set to (it ramps there while playing).
const targetSpeed = computed(() => active.value?.target_speed ?? active.value?.speed ?? 0);
const faderValue = computed(() => dragValue.value ?? Math.round(Math.abs(targetSpeed.value) * 100));
watch(
  targetSpeed,
  (speed) => {
    if (dragValue.value == null && speed != null && speed !== 0) reverse.value = speed < 0;
  },
);
function sendSpeed(percent) {
  return setSpeed((reverse.value ? -Math.min(100, percent) : percent) / 100);
}
let faderTimer = null;
function onFader(event) {
  dragValue.value = Number(event.target.value);
  if (!faderTimer) {
    faderTimer = setTimeout(() => {
      faderTimer = null;
      if (dragValue.value != null) sendSpeed(dragValue.value);
    }, 80);
  }
}
async function onFaderDone(event) {
  clearTimeout(faderTimer);
  faderTimer = null;
  await sendSpeed(Number(event.target.value));
  dragValue.value = null;
}
function onSpeedWheel(event) {
  const step = event.shiftKey ? 1 : 5;
  const delta = (event.deltaY || event.deltaX) < 0 ? step : -step;
  sendSpeed(Math.max(0, Math.min(reverse.value ? 100 : 200, faderValue.value + delta)));
}
function toggleReverse() {
  reverse.value = !reverse.value;
  sendSpeed(faderValue.value);
}
onUnmounted(() => clearTimeout(faderTimer));

const presetOptions = SPEED_PRESETS.map((s) => ({ value: s, label: fmtSpeed(s) }));
const preset = computed(() => SPEED_PRESETS.find((s) => Math.abs(s - Math.abs(targetSpeed.value)) < 0.005) ?? null);
const motionOptions = computed(() =>
  MOTION_MODES.map((m) => ({ ...m, disabled: m.value === "interpolate" && !state.status?.gpu, title: m.value === "interpolate" && !state.status?.gpu ? "needs a GPU: the channel blends instead" : "" })),
);
const tcOptions = [
  { value: "source", label: "Source", title: "the recording time of the frame shown" },
  { value: "output", label: "Output", title: "the time of day of the output" },
];

function onMonitorWheel(event) {
  const d = event.deltaY || event.deltaX;
  if (!d) return;
  if (event.shiftKey) scrubSeconds(d > 0 ? 1 : -1);
  else scrubFrames(d > 0 ? 1 : -1);
}

function onAction(action, value) {
  if (action === "digit") {
    if (cameras.value.some((c) => c.index === value)) setAngle(value);
    return true;
  }
  if (action === "createClip") {
    if (canCreate.value) creating.value = true;
    return true;
  }
  return action === "cue" || action === "nextBank" || action === "prevBank";
}
defineExpose({ onAction });

const recordKind = (cam) => (!cam.record ? "neutral" : cam.recording ? "ok" : "bad");
const recordText = (cam) => (!cam.record ? "OFF" : cam.recording ? "REC" : "NO SIGNAL");
</script>

<template>
  <div class="toolbar">
    <ChannelPicker />
    <span class="spacer"></span>
    <span v-if="active" class="muted small">Interpolation {{ state.status?.gpu ? `${state.status.flow} · ${state.status.preset}` : "off (no GPU)" }}</span>
  </div>

  <div v-if="active" class="lsm">
    <div class="lsm-left">
      <div class="monitor-wrap" @wheel.prevent="onMonitorWheel">
        <LivePicture :src="`/api/v1/channels/${active.index}/preview.jpg`" :tile="`ch${active.index}`" :interval="200" :bump="state.previewBump" alt="channel monitor">
          <span class="ov tl">
            <span class="state-tag" :class="channelState(active)">{{ channelState(active).toUpperCase() }}</span>
            <span class="swatch" :style="{ background: cameraColour(active.camera) }"></span>{{ cameraLabel(active.camera) }}
          </span>
          <span class="ov tr num">{{ fmtSpeed(active.speed) }}</span>
        </LivePicture>
      </div>
      <div class="infostrip">
        <span :title="active.tc_mode === 'source' ? 'source timecode: when the frame was recorded' : 'output timecode: time of day'">
          <span class="bigtc">{{ active.timecode || "--:--:--:--" }}</span>
          <span class="k" style="margin-left: 0.4rem">{{ active.tc_mode === "source" ? "source" : "output" }}</span>
        </span>
        <span><span class="k">IN</span><span class="v mono">{{ active.has_in ? active.in_tc : "–" }}</span></span>
        <span><span class="k">OUT</span><span class="v mono">{{ active.has_out ? active.out_tc : "–" }}</span></span>
        <span><span class="k">Length</span><span class="v">{{ markedNs != null ? fmtNs(markedNs) : "–" }}</span></span>
        <span><span class="k">Motion</span><span class="v">{{ active.motion }}</span></span>
        <span><span class="k">Audio</span><span class="v">{{ active.audio }}</span></span>
        <span v-if="playingClip"><span class="k">Clip</span><span class="v">{{ playingClip.name }}</span></span>
      </div>
      <div class="cams" role="group" aria-label="Camera">
        <button
          v-for="cam in cameras"
          :key="cam.index"
          class="cam"
          :class="{ active: active.camera === cam.index }"
          :aria-pressed="active.camera === cam.index"
          :title="`Switch ${active.label} to ${cam.label} (key ${cam.index})`"
          @click="setAngle(cam.index)"
        >
          <LivePicture :src="`/api/v1/cameras/${cam.index}/preview.jpg`" :tile="`cam${cam.index}`" :interval="1000" :alt="cam.label" />
          <span class="cam-label">
            <kbd v-if="cam.index < 10">{{ cam.index }}</kbd>
            <span class="swatch" :style="{ background: cam.colour }"></span>
            {{ cam.label }}
            <span class="spacer"></span>
            <span class="pill" :class="recordKind(cam)">{{ recordText(cam) }}</span>
          </span>
        </button>
      </div>
    </div>

    <div class="lsm-right">
      <div class="panel">
        <h3>Transport</h3>
        <div class="btn-grid four">
          <button class="btn mark-in" title="Mark IN at the current position" @click="marks('in')">IN</button>
          <button class="btn mark-out" title="Mark OUT at the current position" @click="marks('out')">OUT</button>
          <button class="btn secondary" :disabled="!active.has_in" @click="marks('goto-in')">Go IN</button>
          <button class="btn secondary" :disabled="!active.has_out" @click="marks('goto-out')">Go OUT</button>
          <button class="btn secondary" @click="scrubSeconds(-1)">−1 s</button>
          <button class="btn secondary" @click="scrubFrames(-1)">−1 f</button>
          <button class="btn secondary" @click="scrubFrames(1)">+1 f</button>
          <button class="btn secondary" @click="scrubSeconds(1)">+1 s</button>
        </div>
        <div class="btn-grid three" style="margin-top: 0.4rem">
          <button class="btn tall" :aria-pressed="active.playing" @click="transport('play')">Play</button>
          <button class="btn secondary tall" :aria-pressed="!active.playing && !active.live" @click="transport('pause')">Pause</button>
          <button class="btn live tall" :aria-pressed="active.live" @click="transport('live')">Live</button>
        </div>
        <button class="btn" style="width: 100%; margin-top: 0.4rem" :disabled="!canCreate" :title="canCreate ? '' : 'Mark IN and OUT first'" @click="creating = true">
          Create clip…
        </button>
      </div>

      <div class="panel" @wheel.prevent="onSpeedWheel">
        <h3>Speed</h3>
        <div class="speedrow">
          <span class="speedval">{{ reverse ? "−" : "" }}{{ faderValue }}%</span>
          <input
            type="range"
            min="0"
            :max="reverse ? 100 : 200"
            step="1"
            :value="faderValue"
            aria-label="Speed"
            @input="onFader"
            @change="onFaderDone"
          />
          <button class="btn small secondary" :aria-pressed="reverse" title="Play backwards (down to −100 %)" @click="toggleReverse">Reverse</button>
        </div>
        <div style="margin-top: 0.5rem">
          <Segmented :options="presetOptions" :model-value="preset" fullwidth label="Speed preset" @update:model-value="(s) => setSpeed(reverse ? -s : s)" />
        </div>
      </div>

      <div class="panel">
        <h3>Modes</h3>
        <div class="modes">
          <span>Motion</span>
          <Segmented :options="motionOptions" :model-value="active.motion" fullwidth label="Motion" @update:model-value="(motion) => setMode({ motion })" />
          <span>Audio</span>
          <Segmented :options="AUDIO_MODES" :model-value="active.audio" fullwidth label="Audio" @update:model-value="(audio) => setMode({ audio })" />
          <span>Timecode</span>
          <Segmented :options="tcOptions" :model-value="active.tc_mode" fullwidth label="Timecode" @update:model-value="(timecode) => setMode({ timecode })" />
          <template v-if="active.index > 1">
            <span>Lock</span>
            <button class="btn secondary" :aria-pressed="active.lock" @click="channelPost('lock', { enable: !active.lock })">
              {{ active.lock ? "Following" : "Follow" }} {{ state.status.channels[0].label }} position
            </button>
          </template>
        </div>
      </div>

      <div class="hint">
        Keys: <kbd>{{ keyLabel(keymap.markIn) }}</kbd> IN · <kbd>{{ keyLabel(keymap.markOut) }}</kbd> OUT · <kbd>{{ keyLabel(keymap.playPause) }}</kbd> play/pause ·
        <kbd>{{ keyLabel(keymap.frameBack) }}</kbd> <kbd>{{ keyLabel(keymap.frameForward) }}</kbd> frame · <kbd>{{ keyLabel(keymap.secondBack) }}</kbd>
        <kbd>{{ keyLabel(keymap.secondForward) }}</kbd> second · <kbd>{{ keyLabel(keymap.speedUp) }}</kbd> <kbd>{{ keyLabel(keymap.speedDown) }}</kbd> speed ·
        <kbd>1</kbd>–<kbd>9</kbd> camera · <kbd>{{ keyLabel(keymap.live) }}</kbd> live · <kbd>{{ keyLabel(keymap.createClip) }}</kbd> clip.
        Mouse wheel on the monitor or the timeline: ±1 frame (Shift ±1 s); on the speed panel: ±5 %.
      </div>
    </div>

    <div class="lsm-bottom panel">
      <Timeline />
    </div>
  </div>
  <div v-else class="empty">Waiting for the replay status…</div>

  <CreateClipDialog :open="creating" @close="creating = false" />
</template>
