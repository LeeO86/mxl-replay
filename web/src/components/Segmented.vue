<script setup>
// A row of exclusive buttons (modes, presets, channels, banks); the active one is pressed.
defineProps({
  options: { type: Array, required: true }, // [{ value, label, title?, disabled? }]
  modelValue: { type: null, default: null },
  big: { type: Boolean, default: false },
  fullwidth: { type: Boolean, default: false },
  label: { type: String, default: "" },
});
defineEmits(["update:modelValue"]);
</script>

<template>
  <div class="seg" :class="{ big, fullwidth }" role="group" :aria-label="label">
    <button
      v-for="o in options"
      :key="String(o.value)"
      type="button"
      :class="{ active: o.value === modelValue }"
      :aria-pressed="o.value === modelValue"
      :disabled="o.disabled"
      :title="o.title"
      @click="$emit('update:modelValue', o.value)"
    >
      <slot :option="o">{{ o.label }}</slot>
    </button>
  </div>
</template>
