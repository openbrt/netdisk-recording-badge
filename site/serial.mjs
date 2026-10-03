import { LineDecoder, wifiCommand } from './protocol.mjs';

export class BadgeSerial {
  constructor(onDisconnect = () => {}) {
    this.onDisconnect = onDisconnect;
    this.pending = null;
    this.busy = false;
  }
  async connect(port) {
    await port.open({ baudRate: 115200 });
    this.port = port;
    this.reader = port.readable.getReader();
    this.writer = port.writable.getWriter();
    this.readTask = this.read();
  }
  async read() {
    const decoder = new LineDecoder();
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        for (const event of decoder.feed(value)) {
          if (this.pending?.types.includes(event.type)) this.pending.resolve(event);
        }
      }
    } catch { /* disconnect handled below; do not show device log text */ }
    finally {
      this.reader.releaseLock();
      this.pending?.reject(new Error('USB 连接已断开，请重新连接。'));
      if (!this.closing) {
        this.onDisconnect();
        // Wait for the reader task to finish before closing the port.
        queueMicrotask(() => this.close().catch(() => {}));
      }
    }
  }
  async query(command, types, timeout = 4000) {
    if (!this.port || this.pending) throw new Error('设备未连接或正在处理上一条命令。');
    let timer;
    const response = new Promise((resolve, reject) => {
      this.pending = { types, resolve, reject };
      timer = setTimeout(() => reject(new Error('设备未响应，请关闭其他串口程序、唤醒工牌后重试。')), timeout);
    });
    // Install rejection handling before an asynchronous USB write can fail.
    const write = this.writer.write(new TextEncoder().encode(command + '\n'));
    try {
      const [, event] = await Promise.all([write, response]);
      return event;
    } finally {
      clearTimeout(timer);
      this.pending = null;
    }
  }
  async status() { return this.query('WIFI INFO', ['wifi']); }
  async configure(ssid, password, onSaved = () => {}, timeout = 30000) {
    const command = wifiCommand(ssid, password);
    if (this.busy) throw new Error('正在连接网络，请稍候。');
    this.busy = true;
    try {
      const state = await this.query('STATE', ['state']);
      if (state.recording || state.playing) throw new Error('请先在工牌上停止录音或回放，再修改网络。');
      const result = await this.query(command, ['saved', 'error']);
      if (result.type === 'error') throw new Error('工牌未保存网络，请检查输入后重试。');
      onSaved();
      const end = Date.now() + timeout;
      while (Date.now() < end) {
        const wifi = await this.status();
        // A prior connection is not success for a newly requested SSID.
        if (wifi.connected && wifi.ssid === ssid && wifi.ip && wifi.ip !== '0.0.0.0') return wifi;
        await new Promise(resolve => setTimeout(resolve, 1000));
      }
      throw new Error('已保存，但尚未连接成功。确认 2.4 GHz 网络和密码后重试，或更换网络。');
    } finally { this.busy = false; }
  }
  async close() {
    if (this.closing) return this.closing;
    if (!this.port) return;
    this.closing = (async () => {
      this.pending?.reject(new Error('USB 连接已关闭。'));
      try { await this.reader.cancel(); } catch { /* already disconnected */ }
      await this.readTask;
      this.writer.releaseLock();
      try { await this.port.close(); } catch { /* device may be unplugged */ }
      this.port = null;
    })();
    await this.closing;
    this.closing = null;
  }
}
