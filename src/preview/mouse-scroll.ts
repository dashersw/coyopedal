// Desktop pointer support for previewing the device's touch scrolling.
// Real touch uses browser scrolling; the ESP32 uses Gea's native scroll engine.
export function installMouseScroll(root: HTMLElement) {
  let area: HTMLElement | null = null
  let pressed = false
  let dragged = false
  let suppressClick = false
  let startY = 0
  let previousY = 0
  let previousTime = 0
  let velocity = 0
  let animation = 0

  function stop() {
    cancelAnimationFrame(animation)
    animation = 0
    velocity = 0
  }
  function coast(now: number) {
    if (!area) return
    const dt = Math.min(40, now - previousTime)
    previousTime = now
    const before = area.scrollTop
    area.scrollTop += velocity * dt
    velocity *= Math.exp(-dt / 190)
    if (Math.abs(velocity) < 0.018 || area.scrollTop === before) {
      stop()
      return
    }
    animation = requestAnimationFrame(coast)
  }
  root.addEventListener(
    'pointerdown',
    (event) => {
      if (event.pointerType !== 'mouse') return
      const target = event.target as HTMLElement
      const next = target.closest<HTMLElement>('.scroll')
      if (!next || target.closest('.slider')) {
        stop()
        pressed = suppressClick = false
        area = null
        return
      }
      suppressClick = Math.abs(velocity) > 0.04
      stop()
      area = next
      pressed = true
      dragged = false
      startY = previousY = event.clientY
      previousTime = performance.now()
    },
    true,
  )
  window.addEventListener('pointermove', (event) => {
    if (!pressed || !area) return
    if (!dragged && Math.abs(event.clientY - startY) < 6) return
    dragged = suppressClick = true
    event.preventDefault()
    const now = performance.now()
    const scale = root.clientWidth / 251
    const delta = (previousY - event.clientY) / scale
    const before = area.scrollTop
    area.scrollTop += delta
    velocity =
      area.scrollTop === before
        ? 0
        : velocity * 0.35 + (delta / Math.max(1, now - previousTime)) * 0.65
    previousY = event.clientY
    previousTime = now
  })
  window.addEventListener('pointerup', () => {
    if (!pressed) return
    pressed = false
    if (dragged && performance.now() - previousTime <= 90) {
      previousTime = performance.now()
      animation = requestAnimationFrame(coast)
    }
  })
  root.addEventListener(
    'click',
    (event) => {
      if (!suppressClick) return
      event.preventDefault()
      event.stopImmediatePropagation()
      suppressClick = false
    },
    true,
  )
  root.addEventListener('wheel', stop, { passive: true })
}
