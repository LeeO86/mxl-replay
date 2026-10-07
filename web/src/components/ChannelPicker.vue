<script setup>
// The playout channel the page controls, with each channel's state.
import { computed } from "vue";
import Segmented from "./Segmented.vue";
import { channels, channelState, selectChannel, state } from "../store.js";

const STATE_TEXT = { live: "LIVE", playing: "PLAY", paused: "PAUSE", cued: "CUED", ended: "END", black: "BLACK", idle: "" };
const options = computed(() => channels.value.map((c) => ({ value: c.index, label: c.label, state: channelState(c) })));
</script>

<template>
  <div class="group">
    <span class="caption">Channel</span>
    <Segmented :options="options" :model-value="state.channel" big label="Channel" @update:model-value="selectChannel">
      <template #default="{ option }">
        {{ option.label }}<span class="chan-state" :class="option.state">{{ STATE_TEXT[option.state] }}</span>
      </template>
    </Segmented>
  </div>
</template>
