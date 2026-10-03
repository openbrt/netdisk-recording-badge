import { BadgeSerial } from './serial.mjs';
const $ = id => document.getElementById(id);
let client;
let working = false;
const status = message => { $('status').textContent = message; };
function controls() {
  $('connect').disabled = working || Boolean(client);
  $('disconnect').disabled = working || !client;
  $('save').disabled = working || !client;
  $('refresh').disabled = working || !client;
  $('installer').querySelector('button').disabled = working || Boolean(client);
  $('mode').disabled = working || Boolean(client);
}
function showWifi(info) {
  status(info.connected ? `已连接：${info.ssid} · IP ${info.ip}` : '工牌尚未连接 Wi-Fi。填写网络名称和密码后保存。');
}
async function run(task) {
  working = true; controls();
  try { await task(); }
  catch (error) { status(error.name === 'NotFoundError' ? '未选择设备，可重新点击连接。' : error.message); }
  finally { working = false; controls(); }
}
$('mode').addEventListener('change', () => {
  $('installer').setAttribute('manifest', $('mode').value === 'update' ? 'manifest-update.json' : 'manifest.json');
  $('impact').textContent = $('mode').value === 'update'
    ? '仅适用于本项目 0.4.x、应用位于 0x10000 的兼容分区。安装时不要勾选擦除；应用更新不写入 NVS。其他固件请用首次安装。'
    : '首次安装会替换原固件，合并镜像会覆盖 NVS 配网和网盘授权。需要保留配置时，请确认兼容性并选择仅更新应用。';
});
$('connect').addEventListener('click', () => run(async () => {
  if (!navigator.serial) throw new Error('此浏览器不支持 USB 串口，请在电脑 Chrome 或 Edge 中打开。');
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x303A, usbProductId: 0x1001 }] });
  const candidate = new BadgeSerial(() => {
    client = null; $('password').value = ''; status('USB 已断开，请重新连接。'); controls();
  });
  try { await candidate.connect(port); client = candidate; showWifi(await client.status()); }
  catch (error) { await candidate.close(); client = null; throw error; }
}));
$('disconnect').addEventListener('click', () => run(async () => {
  await client.close(); client = null; $('password').value = ''; status('USB 已断开。');
}));
$('refresh').addEventListener('click', () => run(async () => showWifi(await client.status())));
$('wifi').addEventListener('submit', event => {
  event.preventDefault();
  if (!client || working) return;
  const password = $('password').value;
  $('password').value = '';
  run(async () => {
    status('正在保存并连接网络…');
    showWifi(await client.configure($('ssid').value, password, () => status('已保存，正在等待工牌连接…')));
  });
});
if (!navigator.serial) status('当前浏览器不支持 USB 串口。请使用电脑 Chrome 或 Edge，并通过 USB 数据线连接工牌。');
controls();
