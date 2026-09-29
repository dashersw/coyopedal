// Device benchmark adapted from geastack/examples/apps/bouncing-balls-jsx.
// Keep the same 64-ball motion, JSX style updates and half-second FPS badge.
import { Component, Store, mount } from '@geastack/core'
import './bouncing-balls-benchmark.css'

declare const __gea_Display: {
  setVSync(on: boolean): void
  setFrameRate(fps: number): void
  setTextRasterCache(on: boolean): void
}

interface Ball {
  x: int
  y: int
  dx: int
  dy: int
  color: string
}

class BallStore extends Store {
  balls: Ball[] = [{ x: 0, y: 0, dx: 0, dy: 0, color: '#000000' }]
  fpsText = 'FPS: --'
  fpsWindowStartMs = 0
  fpsWindowFrames = 0

  init() {
    const colors = [
      '#FF0000',
      '#00FF00',
      '#0000FF',
      '#FFFF00',
      '#00FFFF',
      '#FF00FF',
      '#FF6600',
      '#FFFFFF',
      '#8800FF',
      '#FFD700',
    ]
    while (this.balls.length < 64) {
      this.balls.push({ x: 0, y: 0, dx: 0, dy: 0, color: '#000000' })
    }
    const maxX = Math.max(17, Math.floor(window.innerWidth)) - 16
    const maxY = Math.max(17, Math.floor(window.innerHeight)) - 16
    for (let i = 0; i < 64; i++) {
      const seed = i * 97 + 23
      this.balls[i].x = (i * 79 + 17) % (maxX + 1)
      this.balls[i].y = (i * 97 + 31) % (maxY + 1)
      this.balls[i].dx = (((i * 37 + 11) % 7) + 1) * (seed % 2 === 0 ? 1 : -1)
      this.balls[i].dy = (((i * 53 + 17) % 7) + 1) * (seed % 3 === 0 ? 1 : -1)
      this.balls[i].color = colors[i % 10]
    }
  }

  tick(timestampMs: number) {
    if (this.fpsWindowStartMs === 0) this.fpsWindowStartMs = timestampMs
    this.fpsWindowFrames++
    const elapsedMs = timestampMs - this.fpsWindowStartMs
    if (elapsedMs >= 500) {
      this.fpsText = `FPS: ${Math.round((this.fpsWindowFrames * 1000) / elapsedMs)}`
      this.fpsWindowStartMs = timestampMs
      this.fpsWindowFrames = 0
    }
    const maxX = Math.max(17, Math.floor(window.innerWidth)) - 16
    const maxY = Math.max(17, Math.floor(window.innerHeight)) - 16
    for (let i = 0; i < this.balls.length; i++) {
      if (this.balls[i].x + this.balls[i].dx < 0 || this.balls[i].x + this.balls[i].dx > maxX)
        this.balls[i].dx = -this.balls[i].dx
      if (this.balls[i].y + this.balls[i].dy < 0 || this.balls[i].y + this.balls[i].dy > maxY)
        this.balls[i].dy = -this.balls[i].dy
      this.balls[i].x += this.balls[i].dx
      this.balls[i].y += this.balls[i].dy
      if (this.balls[i].x < 0) this.balls[i].x = 0
      if (this.balls[i].x > maxX) this.balls[i].x = maxX
      if (this.balls[i].y < 0) this.balls[i].y = 0
      if (this.balls[i].y > maxY) this.balls[i].y = maxY
    }
  }
}

const balls = new BallStore()

class BouncingBalls extends Component {
  template() {
    return (
      <div class="benchmark-app">
        <span class="benchmark-fps">{balls.fpsText}</span>
        <div class="benchmark-field">
          {balls.balls.map((ball) => (
            <div
              class="benchmark-ball"
              style={{ left: ball.x, top: ball.y, backgroundColor: ball.color }}
            />
          ))}
        </div>
      </div>
    )
  }
}

__gea_Display.setVSync(false)
__gea_Display.setFrameRate(60)
__gea_Display.setTextRasterCache(true)
balls.init()
mount(BouncingBalls)
requestAnimationFrame(function loop(timestampMs) {
  balls.tick(timestampMs)
  requestAnimationFrame(loop)
})
