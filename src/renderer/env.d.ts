import type { CluiAPI } from '../preload/index'

declare module '*.mp3' {
  const src: string
  export default src
}

declare global {
  interface Window {
    clui: CluiAPI
    /** Linux/WebKitGTK: set Wayland input region for click-through */
    __cluiSetInputRegion?: (x: number, y: number, width: number, height: number) => void
  }
}
