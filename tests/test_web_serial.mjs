import { test } from 'node:test';
import assert from 'node:assert/strict';
import { LineDecoder, parseLine, wifiCommand } from '../site/protocol.mjs';
import { BadgeSerial } from '../site/serial.mjs';
const bytes = text => new TextEncoder().encode(text);

function fixture(respond) {
  let stream;
  let released = false;
  const commands = [];
  const port = {
    readable: new ReadableStream({ start(controller) { stream = controller; } }),
    writable: new WritableStream({ write(data) {
      const command = new TextDecoder().decode(data).trimEnd();
      commands.push(command);
      const result = respond(command);
      if (result) {
        // Exercise fragmented USB transfers, including split UTF-8 sequences.
        const data = bytes(result + '\r\n');
        stream.enqueue(data.slice(0, 3)); stream.enqueue(data.slice(3));
      }
    } }),
    async open(options) { assert.equal(options.baudRate, 115200); },
    async close() { released = true; }
  };
  return { port, commands, unplug: () => stream.error(new Error('removed')), closed: () => released };
}
const idle = 'STATE: page=0 rec=0 play=0 off=0 wifi=1 bd=2 bytes=0 ms=0';
const connected = ssid => `WIFI: mode=STA connected=1 saved=1 ssid=${ssid} ip=192.168.1.10 rssi=-40`;

test('credentials preserve case, spaces and password pipes; reject command injection and unsupported SSIDs', () => {
  assert.equal(wifiCommand('My Network', ' Abc|def '), 'WIFI SET My Network| Abc|def ');
  assert.doesNotThrow(() => wifiCommand('界'.repeat(10), '12345678'));
  for (const name of ['', '界'.repeat(11), 'X|Y', ' Leading', 'X\nBAIDU AUTH', 'X\0Y'])
    assert.throws(() => wifiCommand(name, '12345678'));
  for (const pass of ['', 'short', 'X\n12345678', '界'.repeat(22), 'g'.repeat(64)])
    assert.throws(() => wifiCommand('Network', pass));
  assert.doesNotThrow(() => wifiCommand('Network', 'a'.repeat(64)));
  assert.doesNotThrow(() => wifiCommand('Network', '界'.repeat(3)));
  assert.throws(() => wifiCommand('Net\twork', '12345678'));
  assert.throws(() => wifiCommand('Network', '12345678\x1b'));
});
test('decoder handles fragmented multibyte SSIDs and suppresses auth logs', () => {
  const decoder = new LineDecoder();
  const data = bytes('BAIDU: code=PRIVATE\r\n' + connected('办公室') + '\r\n');
  const output = [];
  for (const byte of data) output.push(...decoder.feed(Uint8Array.of(byte)));
  assert.deepEqual(output, [{ type: 'wifi', connected: true, ssid: '办公室', ip: '192.168.1.10', rssi: -40 }]);
  assert.deepEqual(parseLine('WIFI: mode=OTHER connected=0 saved=0 ssid= ip= rssi=0'), {type: 'wifi', connected: false, ssid: '', ip: '', rssi: 0});
  assert.equal(parseLine('WIFI: saved, connecting...').type, 'saved');
  assert.equal(parseLine('WIFI: usage SET <ssid>|<pass>').type, 'error');
});
test('configuration checks idle state and waits for actual connection to requested SSID', async () => {
  let queries = 0;
  const mock = fixture(command => command === 'STATE' ? idle : command.startsWith('WIFI SET') ? 'WIFI: saved, connecting...' : connected(++queries === 1 ? 'Old Network' : 'New Network'));
  const badge = new BadgeSerial(); await badge.connect(mock.port);
  let saved = false;
  const result = await badge.configure('New Network', '12345678', () => { saved = true; });
  assert.ok(saved); assert.equal(result.ssid, 'New Network'); assert.equal(queries, 2);
  assert.deepEqual(mock.commands.slice(0, 2), ['STATE', 'WIFI SET New Network|12345678']);
  await badge.close(); assert.ok(mock.closed());
  assert.equal(mock.port.readable.locked, false); assert.equal(mock.port.writable.locked, false);
});
test('recording or playback prevents network changes', async () => {
  for (const state of [idle.replace('rec=0', 'rec=1'), idle.replace('play=0', 'play=1')]) {
    const mock = fixture(() => state); const badge = new BadgeSerial(); await badge.connect(mock.port);
    await assert.rejects(badge.configure('Network', '12345678'), /停止录音或回放/u);
    assert.deepEqual(mock.commands, ['STATE']); await badge.close();
  }
});
test('query timeout permits retry and unplug rejects pending requests with released locks', async () => {
  let respond = false;
  const mock = fixture(() => respond ? connected('Network') : null);
  const badge = new BadgeSerial(); await badge.connect(mock.port);
  await assert.rejects(badge.query('WIFI INFO', ['wifi'], 10), /未响应/u);
  respond = true; assert.equal((await badge.status()).connected, true);
  respond = false;
  const pending = badge.status(); mock.unplug();
  await assert.rejects(pending, /断开/u); await badge.close();
  assert.ok(mock.closed()); assert.equal(mock.port.readable.locked, false);
});
test('saving credentials alone never constitutes connection success', async () => {
  const mock = fixture(command => command === 'STATE' ? idle : 'WIFI: saved, connecting...');
  const badge = new BadgeSerial(); await badge.connect(mock.port);
  await assert.rejects(badge.configure('Network', '12345678', () => {}, 0), /尚未连接成功/u);
  await badge.close();
});
