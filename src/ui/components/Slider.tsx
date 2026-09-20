import { Component, type GeaElement, type PointerEvent } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import './Slider.css'

export class Slider extends Component<GeaElement, { index: number }> {
  template({ index }: { index: number }) {
    return (
      <div
        class="slider"
        role="slider"
        aria-label={store.parameterNames[index]}
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={Math.round(store.parameterValues[index] * 100)}
        onPointerDown={(event: PointerEvent) =>
          store.sliderDown(index, event.clientX, event.clientY)
        }
        onPointerMove={(event: PointerEvent) =>
          store.sliderMove(index, event.clientX, event.clientY)
        }
        onPointerUp={(event: PointerEvent) => store.sliderUp(index, event.clientX, event.clientY)}
      >
        <div class="slider-track" />
        <div
          class="slider-fill"
          style={{ width: `${store.parameterValues[index] * 100}%`, backgroundColor: store.accent }}
        />
        <div
          class="slider-thumb"
          style={{ left: `${store.parameterValues[index] * 100}%`, backgroundColor: store.accent }}
        />
      </div>
    )
  }
}
