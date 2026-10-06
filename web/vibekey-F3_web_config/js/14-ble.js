/* ===== 蓝牙(BLE) 传输层 + 连接方式下拉 (固件 0xFF00 配置通道) =====
   从 assemble_index.py 的内嵌字符串拆出(2026-10-03)。目的: 让这些逻辑成为可独立
   编辑/语法高亮/逐文件 node --check 的 .js，而不是 1200 行 Python 字符串。
   拼接顺序 = 文件名序号，勿调整。 */
// ===== 蓝牙(BLE) 传输层 —— 让配置读写也能走无线 =====
// 固件把改键配置协议开放在 0xFF00 服务下(Main.qml 侧对应 workers/ble_config_worker.py):
//   0xFF03  写 CONF 命令帧(最大 277B)
//   0xFF04  notify 回包帧(最大 276B)
// 关键设计: 这里把 BLE 特征包装成一个「Web Serial 端口形状」的对象
// (readable=ReadableStream, writable.getWriter, setSignals, close),
// 于是 readLoop / writer.write / verifyVibeKey / onPortOpened / 断线重连
// **一行都不用改**, 两条通道共用同一套 CONF 帧组装与解析, 永不漂移。
const BLE_SVC_UUID = 0xff00, BLE_TX_UUID = 0xff03, BLE_RX_UUID = 0xff04;
let bleDev = null, bleCharTx = null, bleCharRx = null, bleStreamCtl = null;
function bleUuid16(n) { return '0000' + n.toString(16).padStart(4, '0') + '-0000-1000-8000-00805f9b34fb'; }

function makeBlePortShim() {
  return {
    readable: new ReadableStream({ start(c) { bleStreamCtl = c; } }),
    writable: {
      getWriter: () => ({
        write: async (data) => {
          if (!bleCharTx) throw new Error('蓝牙未连接');
          const buf = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
          // 无响应写更快; 若特征不支持再回退到普通写
          if (bleCharTx.writeValueWithoutResponse) {
            try { await bleCharTx.writeValueWithoutResponse(buf); return; } catch (e) { /* 回退 */ }
          }
          await bleCharTx.writeValue(buf);
        },
        releaseLock() {}, close() {},
      }),
    },
    open: async () => {},
    setSignals: async () => {},
    close: async () => { try { bleDev.gatt.disconnect(); } catch (e) {} },
  };
}

async function connectDeviceBle() {
  if (!('bluetooth' in navigator)) {
    toast('当前浏览器不支持 Web Bluetooth，请用 Chrome / Edge', 'error');
    log('浏览器不支持 Web Bluetooth API', 'error');
    return;
  }
  if (!window.isSecureContext) {
    toast('蓝牙连接需要 HTTPS 环境（本地请用 localhost）', 'error');
    return;
  }
  try {
    log('请求配对 VibeKey-F3 蓝牙设备…', 'info');
    // 不用 services 过滤: 设备广播里不带 0xFF00, 过滤会导致 Chrome「找不到兼容设备」
    bleDev = await navigator.bluetooth.requestDevice({
      acceptAllDevices: true, optionalServices: [BLE_SVC_UUID],
    });
    const server = await bleDev.gatt.connect();
    const svc = await server.getPrimaryService(BLE_SVC_UUID);
    bleCharTx = await svc.getCharacteristic(bleUuid16(BLE_TX_UUID));
    bleCharRx = await svc.getCharacteristic(bleUuid16(BLE_RX_UUID));
    await bleCharRx.startNotifications();
    bleCharRx.addEventListener('characteristicvaluechanged', (ev) => {
      const v = ev.target.value;
      if (v && bleStreamCtl) {
        bleStreamCtl.enqueue(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
      }
    });
    bleDev.addEventListener('gattserverdisconnected', () => {
      if (port && port.__ble) { log('蓝牙已断开', 'warn'); disconnectDevice(); }
    });
    const shim = makeBlePortShim();
    shim.__ble = true;
    port = shim;
    log('已连接蓝牙: ' + (bleDev.name || 'VibeKey-F3') + '（配置通道 0xFF00）', 'success');
    toast('蓝牙连接成功', 'success');
    await onPortOpened();
  } catch (e) {
    log('蓝牙连接失败: ' + e.message, 'error');
    toast('蓝牙连接失败: ' + e.message, 'error');
  }
}

// ---- 连接方式下拉 ----
function toggleConnMenu(e) {
  if (e) e.stopPropagation();
  const d = document.getElementById('connDropdown');
  if (d) d.classList.toggle('open');
}
function closeConnMenu() {
  const d = document.getElementById('connDropdown');
  if (d) d.classList.remove('open');
}
function pickConn(kind) {
  closeConnMenu();
  if (kind === 'ble') connectDeviceBle();
  else connectDevice();
}
document.addEventListener('click', (e) => {
  if (!e.target.closest || !e.target.closest('.conn-menu')) closeConnMenu();
});
