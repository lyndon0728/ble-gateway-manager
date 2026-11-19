import { requestId, utf8 } from './security2.js';

const PHASES = {
  NONE: '尚无本地升级记录', RECEIVING: '正在接收固件', READY: '校验通过，等待安装',
  REBOOT_PENDING: '即将重启试运行', CONFIRMING: '正在确认启动自检结果', CONFIRMED: '新版本已通过启动自检',
  ABORTED: '上传已取消', FAILED: '升级失败', ROLLED_BACK: '已回退到原版本', UNKNOWN: '状态待确认，请查询设备'
};
const ERRORS = {
  BUSY: '设备正在执行其他维护或网络操作，请稍后查询升级状态。',
  NOT_READY: '设备尚未准备好升级，请稍后查询状态。',
  CONFLICT: '升级记录、文件或当前状态不匹配，请查询设备状态；如需更换文件，请先取消原上传。',
  SESSION_EXPIRED: '设置窗口已到期，请重新打开热点并登录。未提交的上传需要重新开始。',
  INTEGRITY_ERROR: '固件签名或完整性校验失败，请重新获取配套的发布包。',
  INVALID_SIGNATURE: '固件签名校验失败，请使用设备信任的发布包。',
  VERSION_MISMATCH: '固件版本与签名清单不一致，请重新获取配套的发布包。',
  INVALID_ARGUMENT: '升级包或请求不符合要求，请检查型号、版本和所选文件。',
  STORAGE_FAULT: '设备存储操作失败，请查询状态并联系维护人员。'
};
const TERMINAL = new Set(['NONE', 'CONFIRMED', 'ABORTED', 'FAILED', 'ROLLED_BACK']);
const STORAGE_KEY = 'gw32.otaUploadId';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// These checks improve file-selection feedback. Signature and image validation
// remain authoritative on the device before any boot partition is selected.
export function readVerification(text, imageSize) {
  if (utf8.encode(text).length > 24576) throw Error('签名文件过大。');
  const verification = JSON.parse(text);
  if (!verification || typeof verification.manifest !== 'string' ||
      utf8.encode(verification.manifest).length > 8192 ||
      typeof verification.signature !== 'string' ||
      !/^[A-Za-z0-9+/]{86}==$/.test(verification.signature))
    throw Error('请选择包含 manifest 原文和 signature 的签名文件。');
  const manifest = JSON.parse(verification.manifest);
  if (!manifest || manifest.kind !== 'FIRMWARE' || manifest.component !== 'GATEWAY' ||
      manifest.imageFormat !== 'ESP_IDF_APP_BIN')
    throw Error('此页面只接受 ESP32-S3 的应用固件包。');
  if (!Number.isSafeInteger(manifest.sizeBytes) || manifest.sizeBytes < 1 ||
      manifest.sizeBytes > 0x280000 || manifest.sizeBytes !== imageSize)
    throw Error('固件文件长度与签名清单不一致，或超过应用分区容量。');
  return { manifest: verification.manifest, signature: verification.signature };
}

export function checkedOffset(status, uploadId, imageSize) {
  if (status.uploadId !== uploadId || status.sizeBytes !== imageSize ||
      !Number.isSafeInteger(status.offset) || status.offset < 0 || status.offset > imageSize)
    throw Error('设备返回的上传状态与本次固件不匹配，请查询升级状态。');
  return status.offset;
}

export function encodeChunk(bytes) {
  if (bytes.length < 1 || bytes.length > 4096) throw Error('无效的固件分片长度。');
  return btoa(String.fromCharCode(...bytes));
}

export class LocalOtaUi {
  constructor({ $, call, run, notice }) {
    this.$ = $; this.run = run; this.notice = notice;
    this.uploadId = sessionStorage.getItem(STORAGE_KEY) || '';
    this.status = {}; this.uploading = false; this.cancelRequested = false;
    this.call = async (operation, body) => {
      // Never leave an old READY state actionable after an ambiguous response.
      // A fresh status query is required before a later installation attempt.
      try { return await call(operation, body); }
      catch (error) {
        this.render({phase:'UNKNOWN', uploadId:this.uploadId});
        if (ERRORS[error.code]) error.message = ERRORS[error.code];
        throw error;
      }
    };
    $('ota-upload').addEventListener('click', () => run(() => this.upload()));
    $('ota-status').addEventListener('click', () => run(() => this.refresh()));
    $('ota-install').addEventListener('click', () => run(() => this.install()));
    $('ota-abort').addEventListener('click', () => {
      if (this.uploading) {
        this.cancelRequested = true;
        this.notice('正在取消：当前分片结束后停止上传。');
      } else run(() => this.abort());
    });
  }
  remember(id) {
    if (!id) return;
    this.uploadId = id; sessionStorage.setItem(STORAGE_KEY, id);
  }
  render(status) {
    this.status = status;
    this.remember(status.uploadId);
    const label = PHASES[status.phase] || status.phase || '状态未知';
    this.$('ota-state').textContent = `${label}${status.version ? ' · 版本 ' + status.version : ''}`;
    this.$('ota-id').textContent = status.uploadId ? `升级编号：${status.uploadId}` : '';
    const total = Number.isSafeInteger(status.sizeBytes) ? status.sizeBytes : 0;
    const offset = Number.isSafeInteger(status.offset) ? status.offset : 0;
    this.$('ota-progress').max = total || 1;
    this.$('ota-progress').value = offset;
    this.$('ota-bytes').textContent = total ? `${offset} / ${total} 字节（${Math.floor(offset * 100 / total)}%）` : '';
    this.$('ota-install').disabled = this.uploading || status.phase !== 'READY';
    this.$('ota-abort').disabled = !this.uploading && !['RECEIVING', 'READY'].includes(status.phase);
  }
  async refresh() {
    // The latest local journal survives reboot. An old browser ID must not hide
    // the result of an upgrade initiated by another authorized browser.
    this.$('ota-install').disabled = true;
    const result = await this.call('OTA_STATUS', {});
    this.render(result); return result;
  }
  async abort() {
    if (!this.uploadId) throw Error('没有待取消的本地升级。');
    this.render(await this.call('OTA_ABORT', { uploadId: this.uploadId }));
    this.notice('已取消本地升级，继续使用原固件。');
  }
  async upload() {
    const verificationFile = this.$('ota-manifest').files[0], image = this.$('ota-image').files[0];
    if (!verificationFile || !image) throw Error('请同时选择签名文件和 ESP32-S3 固件文件。');
    if (verificationFile.size > 24576) throw Error('签名文件过大。');
    const verification = readVerification(await verificationFile.text(), image.size);
    let current = await this.refresh();
    if (current.uploadId && !TERMINAL.has(current.phase)) this.remember(current.uploadId);
    else this.remember(requestId());
    const begin = { uploadId: this.uploadId, ...verification };
    if (utf8.encode(JSON.stringify(begin)).length > 15500) throw Error('签名清单编码后过大，不能通过本地接口上传。');
    current = await this.call('OTA_BEGIN', begin);
    let offset = checkedOffset(current, this.uploadId, image.size);
    if (!['RECEIVING', 'READY'].includes(current.phase)) {
      this.render(current); throw Error('当前升级不能继续上传，请查询结果。');
    }
    this.uploading = true; this.cancelRequested = false;
    this.$('ota-upload').disabled = true;
    this.$('ota-manifest').disabled = true; this.$('ota-image').disabled = true;
    this.render(current);
    try {
      while (offset < image.size) {
        // Five chunks per second also leave capacity for authentication and
        // status calls under the device's twelve-requests-per-second limit.
        await sleep(200);
        if (this.cancelRequested) { await this.abort(); return; }
        const end = Math.min(offset + 4096, image.size);
        const bytes = new Uint8Array(await image.slice(offset, end).arrayBuffer());
        current = await this.call('OTA_WRITE', { uploadId: this.uploadId, offset, data: encodeChunk(bytes) });
        const received = checkedOffset(current, this.uploadId, image.size);
        if (received !== end) throw Error('设备确认的偏移不正确，请重新登录后查询升级状态。');
        offset = received; this.render(current);
      }
      if (this.cancelRequested) { await this.abort(); return; }
      if (current.phase !== 'READY') throw Error('设备尚未完成固件校验，请查询升级状态。');
      this.notice('固件已通过设备校验。确认目标版本后点击“安装并重启”。');
    } finally {
      this.uploading = false; this.$('ota-upload').disabled = false;
      this.$('ota-manifest').disabled = false; this.$('ota-image').disabled = false;
      this.render(this.status);
    }
  }
  async install() {
    if (this.status.phase !== 'READY' || !this.uploadId) throw Error('请先完成固件上传和校验。');
    if (!confirm(`安装版本 ${this.status.version || '设备已校验的固件'} 并重启网关？重启会短暂中断服务。请保持供电；新固件启动自检失败将回退。`)) return;
    this.$('ota-install').disabled = true;
    // A lost reply is ambiguous: never retry a ciphertext or infer success.
    // Reauthenticate and query the durable device journal after reconnecting.
    this.notice('正在提交安装。若连接断开，请重连热点并登录，查询最终升级状态。');
    this.render(await this.call('OTA_COMMIT', { uploadId: this.uploadId }));
    this.notice('已提交安装，即将重启。重启后按设备设置键重新打开热点，登录并查询升级状态；“重启中”尚不代表升级成功。');
  }
}
