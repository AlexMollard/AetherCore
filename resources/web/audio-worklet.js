// Interleaved stereo PCM is transferred back for reuse. The bounded ring and
// transfer lists are allocated once; process() does not allocate JS objects.
class AetherPcmSink extends AudioWorkletProcessor {
    constructor() {
        super();
        this.slots = Array.from({length: 8}, () => ({data: null, transfer: [null]}));
        this.read = 0;
        this.count = 0;
        this.offset = 0;
        this.stopped = false;
        this.rejectedTransfer = [null];
        this.port.onmessage = event => {
            const data = event.data;
            if (data && data.stop === true) {
                this.stopped = true;
                for (const slot of this.slots) {
                    slot.data = null;
                    slot.transfer[0] = null;
                }
                this.count = 0;
                this.port.onmessage = null;
                this.port.close();
                return;
            }
            if (!(data instanceof Float32Array) || data.length !== 2048 || this.count === this.slots.length) {
                if (data instanceof Float32Array) {
                    this.rejectedTransfer[0] = data.buffer;
                    this.port.postMessage(data, this.rejectedTransfer);
                }
                return;
            }
            const slot = this.slots[(this.read + this.count) % this.slots.length];
            slot.data = data;
            slot.transfer[0] = data.buffer;
            ++this.count;
        };
    }
    process(inputs, outputs) {
        if (this.stopped)
            return false;
        const output = outputs[0];
        for (let i = 0; i < output[0].length; ++i) {
            if (this.count) {
                const slot = this.slots[this.read];
                const data = slot.data;
                output[0][i] = data[this.offset++];
                output[1][i] = data[this.offset++];
                if (this.offset === data.length) {
                    this.offset = 0;
                    this.port.postMessage(data, slot.transfer);
                    slot.data = null;
                    slot.transfer[0] = null;
                    this.read = (this.read + 1) % this.slots.length;
                    --this.count;
                }
            } else {
                output[0][i] = output[1][i] = 0;
            }
        }
        return true;
    }
}
registerProcessor('aether-pcm-sink', AetherPcmSink);
