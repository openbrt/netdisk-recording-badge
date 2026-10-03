export function wifiCommand(ssid, password) {
  const size = new TextEncoder().encode(ssid).length;
  if (!size || size > 32 || (/[|\r\n\0]/u.test(ssid) || ssid.startsWith(' ')))
    throw new Error('网络名称须为 1～32 字节，且不能包含竖线或换行。');
  if (/[\r\n\0]/u.test(password) || !(
    (password.length >= 8 && new TextEncoder().encode(password).length <= 63) ||
    /^[0-9a-f]{64}$/iu.test(password)))
    throw new Error('请输入 8～63 字节的 Wi-Fi 密码，或 64 位十六进制密钥；此入口不支持无密码网络。');
  return `WIFI SET ${ssid}|${password}`;
}

export function parseLine(line) {
  const state = line.match(/^STATE: page=\d+ rec=(\d+) play=(\d+) off=\d+ wifi=(\d+) bd=\d+ bytes=\d+ ms=\d+$/u);
  if (state) return { type: 'state', recording: state[1] !== '0', playing: state[2] !== '0', connected: state[3] !== '0' };
  const wifi = line.match(/^WIFI: mode=\S+ connected=(\d) saved=\d+ ssid=(.*?) ip=([\d.]*) rssi=(-?\d+)$/u);
  if (wifi) return { type: 'wifi', connected: wifi[1] === '1', ssid: wifi[2], ip: wifi[3], rssi: Number(wifi[4]) };
  if (line === 'WIFI: saved, connecting...') return { type: 'saved' };
  if (line.startsWith('WIFI: SET fail') || line.startsWith('WIFI: usage ')) return { type: 'error' };
  return null; // Never expose raw logs: they may contain authorization information.
}

export class LineDecoder {
  decoder = new TextDecoder();
  buffer = '';
  feed(bytes) {
    this.buffer += this.decoder.decode(bytes, { stream: true });
    const lines = this.buffer.split('\n');
    this.buffer = lines.pop().slice(-16384);
    return lines.map(line => parseLine(line.replace(/\r$/u, ''))).filter(Boolean);
  }
}
