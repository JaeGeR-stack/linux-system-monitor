<script setup>
import { ref, computed, onMounted, onUnmounted, nextTick } from 'vue'
import * as echarts from 'echarts'

const API_URL = 'http://127.0.0.1:8080/api/monitor'
const REFRESH_MS = 2000
const thresholds = ref({ cpu: 80, memory: 85, disk: 90 })
const metrics = ref({ cpu: null, memory: null, disk: null, processes: null })
const lastUpdate = ref('尚未收到数据')
const history = ref([])
const alerts = ref([])
const chartEl = ref(null)
const connected = ref(false)
const errorMessage = ref('正在连接 C 语言服务端…')
let chart = null
let timer = null
let resizeObserver = null
let controller = null
let lastSequence = null
let lastTimestamp = null
let lastReceivedAt = 0
let alertStates = { cpu: false, memory: false, disk: false }
let eventId = 0
let disposed = false

const cards = computed(() => [
  { key: 'cpu', name: 'CPU 使用率', icon: '◈', value: metrics.value.cpu, threshold: thresholds.value.cpu, color: '#6d9dff' },
  { key: 'memory', name: '内存使用率', icon: '▤', value: metrics.value.memory, threshold: thresholds.value.memory, color: '#9b86ff' },
  { key: 'disk', name: '磁盘使用率', icon: '◉', value: metrics.value.disk, threshold: thresholds.value.disk, color: '#42cbb2' },
])
const abnormalCount = computed(() => connected.value ? cards.value.filter(item => item.value !== null && item.value >= item.threshold).length : 0)
const statusText = computed(() => !connected.value ? '等待监控数据' : abnormalCount.value ? `${abnormalCount.value} 项指标异常` : '指标正常')
const statusCopy = computed(() => !connected.value ? errorMessage.value : abnormalCount.value ? '部分指标已超过设定阈值，请查看告警记录。' : 'CPU、内存和磁盘使用率均处于设定阈值以下。')
const trendSubtitle = computed(() => `最近 ${history.value.length} 个采样点 · 每 2 秒请求一次`)

function formatTime() {
  return new Date().toLocaleTimeString('zh-CN', { hour12: false })
}
function addAlert(level, message) {
  alerts.value.unshift({ id: ++eventId, level, message, time: formatTime() })
  alerts.value = alerts.value.slice(0, 6)
}
function validPercent(v) {
  return typeof v === 'number' && Number.isFinite(v) && v >= 0 && v <= 100
}
function validThresholds(v) {
  return v && ['cpu', 'memory', 'disk'].every(k => typeof v[k] === 'number' && Number.isFinite(v[k]) && v[k] > 0 && v[k] <= 100)
}
function updateChart() {
  if (!chart) return
  chart.setOption({
    backgroundColor: 'transparent',
    animationDuration: 450,
    tooltip: { trigger: 'axis', backgroundColor: '#18233a', borderColor: '#33415c', textStyle: { color: '#eaf1ff' }, valueFormatter: v => `${v}%` },
    legend: { top: 0, right: 0, textStyle: { color: '#9caecb' }, itemWidth: 12, itemHeight: 7 },
    grid: { left: 42, right: 14, top: 46, bottom: 30 },
    xAxis: { type: 'category', boundaryGap: false, data: history.value.map(p => p.time), axisLabel: { color: '#7f90ad', fontSize: 11 }, axisLine: { lineStyle: { color: '#2b3952' } }, axisTick: { show: false } },
    yAxis: { type: 'value', min: 0, max: 100, axisLabel: { color: '#7f90ad', formatter: '{value}%' }, splitLine: { lineStyle: { color: '#26334b', type: 'dashed' } } },
    series: [
      { name: 'CPU', type: 'line', smooth: true, showSymbol: false, lineStyle: { width: 3 }, itemStyle: { color: '#6d9dff' }, areaStyle: { color: 'rgba(109,157,255,.08)' }, data: history.value.map(p => p.cpu) },
      { name: '内存', type: 'line', smooth: true, showSymbol: false, lineStyle: { width: 3 }, itemStyle: { color: '#9b86ff' }, data: history.value.map(p => p.memory) },
      { name: '磁盘', type: 'line', smooth: true, showSymbol: false, lineStyle: { width: 3 }, itemStyle: { color: '#42cbb2' }, data: history.value.map(p => p.disk) },
    ],
  })
}
function setOffline(message) {
  if (connected.value) addAlert('warn', `数据连接中断：${message}`)
  connected.value = false
  errorMessage.value = message
  alertStates = { cpu: false, memory: false, disk: false }
  lastSequence = null
  lastTimestamp = null
  lastReceivedAt = 0
}
async function poll() {
  if (controller) return
  controller = new AbortController()
  const currentController = controller
  const timeout = setTimeout(() => currentController.abort(), 4000)
  try {
    const response = await fetch(API_URL, { signal: currentController.signal, cache: 'no-store' })
    if (!response.ok) throw new Error(`HTTP ${response.status}`)
    const data = await response.json()
    if (disposed) return
    if (validThresholds(data.thresholds)) thresholds.value = { ...data.thresholds }
    if (data.connected !== true) {
      setOffline('HTTP 接口可访问，但尚未收到 TCP 客户端数据。请启动 ./client。')
      return
    }
    if (![data.cpu_usage, data.memory_usage, data.disk_usage].every(validPercent) || !Number.isInteger(data.process_count) || data.process_count < 0) {
      throw new Error('服务端返回的数据字段不完整或不合法')
    }
    const seq = Number.isInteger(data.sequence) ? data.sequence : null
    const stamp = Number.isFinite(data.timestamp) ? data.timestamp : null
    const isNewSample = seq !== null && stamp !== null
      ? seq !== lastSequence || stamp !== lastTimestamp
      : true
    if (!isNewSample && lastReceivedAt && Date.now() - lastReceivedAt > 10000) {
      setOffline('监控数据超过 10 秒未更新，请检查 C 客户端。')
      return
    }
    if (!connected.value) addAlert('info', '已连接 Linux C 服务端，正在接收真实监控数据')
    connected.value = true
    errorMessage.value = ''
    if (!isNewSample) return
    lastSequence = seq
    lastTimestamp = stamp
    lastReceivedAt = Date.now()
    metrics.value = {
      cpu: data.cpu_usage,
      memory: data.memory_usage,
      disk: data.disk_usage,
      processes: data.process_count,
    }
    lastUpdate.value = formatTime()
    for (const item of cards.value) {
      const isAbnormal = item.value >= item.threshold
      if (isAbnormal && !alertStates[item.key]) addAlert('warn', `${item.name}达到 ${item.value.toFixed(2)}%，超过 ${item.threshold}% 阈值`)
      if (!isAbnormal && alertStates[item.key]) addAlert('success', `${item.name}已恢复正常（${item.value.toFixed(2)}%）`)
      alertStates[item.key] = isAbnormal
    }
    history.value.push({ time: lastUpdate.value, cpu: metrics.value.cpu, memory: metrics.value.memory, disk: metrics.value.disk })
    if (history.value.length > 24) history.value.shift()
    updateChart()
  } catch (error) {
    if (!disposed) setOffline(error.name === 'AbortError' ? 'HTTP 请求超时，请检查服务端。' : `无法获取监控数据：${error.message}`)
  } finally {
    clearTimeout(timeout)
    if (controller === currentController) controller = null
  }
}
function manualRefresh() { void poll() }

onMounted(async () => {
  await nextTick()
  chart = echarts.init(chartEl.value)
  updateChart()
  resizeObserver = new ResizeObserver(() => chart?.resize())
  resizeObserver.observe(chartEl.value)
  await poll()
  if (!disposed) timer = setInterval(poll, REFRESH_MS)
})
onUnmounted(() => {
  disposed = true
  if (timer) clearInterval(timer)
  controller?.abort()
  resizeObserver?.disconnect()
  chart?.dispose()
})
</script>

<template>
  <div class="app-shell">
    <aside class="sidebar">
      <div class="brand"><span class="brand-mark">◈</span><div><strong>Linux Monitor</strong><small>系统监控平台</small></div></div>
      <div class="sidebar-label">工作台</div>
      <div class="nav-item active"><span>▦</span> 监控总览</div>
      <div class="nav-item"><span>◷</span> 历史趋势 <span class="soon">当前会话</span></div>
      <div class="nav-item"><span>♧</span> 告警事件 <span class="soon">当前会话</span></div>
      <div class="sidebar-bottom"><span class="pulse-dot"></span> {{ connected ? '真实数据已接入' : '等待服务端数据' }} <small>v3.0</small></div>
    </aside>

    <main class="main-content">
      <header class="topbar"><div class="breadcrumb">工作台 <span>/</span> <strong>监控总览</strong></div><div class="top-actions"><span class="demo-chip">{{ connected ? '● LIVE DATA' : '● OFFLINE' }}</span><span class="avatar">LM</span></div></header>
      <div class="page-content">
        <div class="page-heading"><div><p class="eyebrow">SYSTEM OVERVIEW</p><h1>系统监控总览</h1><p class="subtitle">实时掌握 Linux 主机资源状态与告警动态</p></div><div class="updated">最后更新 <strong>{{ lastUpdate }}</strong><span class="live-dot"></span></div></div>
        <div class="notice"><span>ⓘ</span><div><strong>{{ connected ? '真实数据监控已启用' : '等待真实监控数据' }}</strong><p>{{ connected ? '数据来自 Linux C 客户端，经 TCP 服务端与 HTTP 接口传输；页面每 2 秒请求一次。' : errorMessage }}</p></div></div>

        <section class="summary-grid">
          <article v-for="item in cards" :key="item.key" class="metric-card">
            <div class="metric-top"><span class="metric-label">{{ item.name }}</span><span class="metric-symbol" :style="{ color: item.color }">{{ item.icon }}</span></div>
            <div class="metric-number">{{ item.value === null ? '--' : item.value.toFixed(2) }}<span>%</span></div>
            <div class="progress-track"><div class="progress-fill" :style="{ width: `${item.value ?? 0}%`, background: item.value !== null && item.value >= item.threshold ? '#fb7185' : item.color }"></div></div>
            <div class="metric-footer"><span>告警阈值 {{ item.threshold }}%</span><span :class="!connected || item.value === null ? 'muted' : item.value >= item.threshold ? 'bad' : 'good'">{{ !connected || item.value === null ? '● 未连接' : item.value >= item.threshold ? '● 异常' : '● 正常' }}</span></div>
          </article>
          <article class="metric-card process-card"><div class="metric-top"><span class="metric-label">运行进程数</span><span class="metric-symbol">▤</span></div><div class="metric-number">{{ metrics.processes ?? '--' }}<span> 个</span></div><div class="process-bottom"><span class="process-indicator">↗</span> Linux 进程统计</div></article>
        </section>

        <div class="content-grid">
          <section class="panel trend-panel"><div class="panel-heading"><div><h2>资源使用趋势</h2><p>{{ trendSubtitle }}</p></div><span class="panel-tag">{{ connected ? '实时数据' : '等待连接' }}</span></div><div ref="chartEl" class="trend-chart"></div></section>
          <section class="panel status-panel"><div class="panel-heading"><div><h2>系统运行状态</h2><p>根据实际监控指标判断</p></div></div><div class="status-icon" :class="{ warning: abnormalCount || !connected }">{{ !connected ? '…' : abnormalCount ? '!' : '✓' }}</div><h3>{{ statusText }}</h3><p class="status-copy">{{ statusCopy }}</p><div class="status-separator"></div><div class="status-row"><span>数据来源</span><strong>Linux C 服务端</strong></div><div class="status-row"><span>请求频率</span><strong>2 秒</strong></div><div class="status-row"><span>服务端连接</span><strong :class="connected ? 'good' : 'muted'">{{ connected ? '已接收数据' : '离线 / 等待数据' }}</strong></div></section>
        </div>

        <section class="panel alerts-panel"><div class="panel-heading"><div><h2>最近告警与事件</h2><p>根据当前会话中的真实采样数据生成，刷新页面后清空</p></div><button class="simulate-btn" type="button" @click="manualRefresh">立即刷新</button></div><div class="events"><div v-if="alerts.length === 0" class="event-row"><div class="event-content"><strong>暂无事件记录</strong><small>等待监控数据</small></div></div><div v-for="event in alerts" :key="event.id" class="event-row"><span class="event-icon" :class="event.level">{{ event.level === 'warn' ? '!' : event.level === 'success' ? '✓' : 'i' }}</span><div class="event-content"><strong>{{ event.message }}</strong><small>{{ event.level === 'warn' ? '异常告警' : event.level === 'success' ? '状态恢复' : '系统事件' }}</small></div><time>{{ event.time }}</time></div></div></section>
        <footer>Linux System Monitor v3.0 · C + TCP + HTTP + Vue 3 + ECharts · 实时监控</footer>
      </div>
    </main>
  </div>
</template>
