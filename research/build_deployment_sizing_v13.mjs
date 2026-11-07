import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Workbook, SpreadsheetFile } from '@oai/artifact-tool';

const research = path.dirname(fileURLToPath(import.meta.url));
const output = path.resolve(research, '../outputs/ble-platform-onprem-v13');
const plan = JSON.parse(await fs.readFile(path.join(research, 'deployment_sizing_v13.json'), 'utf8'));
const workbook = Workbook.create();
const calculator = workbook.worksheets.add('容量测算');
const selection = workbook.worksheets.add('方案选型');
const acceptance = workbook.worksheets.add('验收清单');
const titleColor = '#203B5C';
const headerStyle = { fill: titleColor, font: { name: 'Microsoft YaHei', size: 10, bold: true, color: '#FFFFFF' }, rowHeight: 30, verticalAlignment: 'center', wrapText: true };

function initialize(sheet, lastCell, title, note, widths) {
  sheet.showGridLines = false;
  sheet.getRange(`A1:${lastCell}`).format.font = { name: 'Microsoft YaHei', size: 10, color: '#1B2B3E' };
  sheet.getRange('A2').values = [[title]];
  sheet.getRange('A2').format.font = { bold: true, size: 15 };
  sheet.getRange('A3').values = [[note]];
  sheet.getRange('A2:F2').format.rowHeight = 28;
  sheet.getRange('A3:F3').format.rowHeight = 23;
  widths.forEach((width, index) => {
    sheet.getRangeByIndexes(0, index, 42, 1).format.columnWidth = width;
  });
}

function addTable(sheet, headers, rows, name, rowHeight) {
  const table = sheet.getRangeByIndexes(4, 0, rows.length + 1, headers.length);
  table.values = [headers, ...rows];
  table.format.wrapText = true;
  table.format.verticalAlignment = 'center';
  table.format.rowHeight = rowHeight;
  sheet.getRangeByIndexes(4, 0, 1, headers.length).format = headerStyle;
  sheet.tables.add(table, true, name).showFilterButton = true;
  sheet.freezePanes.freezeRows(5);
}

initialize(calculator, 'F42', '客户服务器容量测算 V1.3', '黄色单元格为可编辑输入。示例与配置均为压测起点。1 GB = 1,000,000,000 字节。', [28, 13, 20, 20, 20, 70]);
calculator.tabColor = titleColor;
calculator.getRange('A5:F5').values = [['输入参数', '单位', ...plan.scenarios.map((item) => item.name), '填写口径']];
calculator.getRange('A5:F5').format = headerStyle;
const inputs = [
  ['在线网关', '台', '同时在线数量。'],
  ['活跃 BLE 设备', '个', '同一统计窗内的活跃源设备数。'],
  ['有效上报间隔', '秒', '按过滤或聚合后的有效事件间隔。'],
  ['转发份数', '倍', '平均每次事件的上行份数，包含覆盖重叠。'],
  ['单事件平均长度', '字节', '含应用封包。链路协议与 TLS 开销另测。'],
  ['本地转发开关', '0 或 1', '0 为网关直连客户接收端。1 为通过本地管理配套转发。'],
  ['目标断链时间', '分钟', '单下游不可达的持续时间。'],
  ['积压清空时限', '分钟', '目标恢复时限。需保留实时处理容量。'],
  ['存储开销系数', '倍', '估算单队列的持久化开销。压测后更新。'],
  ['网关心跳周期', '秒', '管理链路的在线心跳间隔。'],
  ['全量重连窗口', '秒', '估算平均重连速率。短时鉴权峰值另测。'],
  ['单个 OTA 包', 'MiB', '实际下发包大小。'],
  ['OTA 同批下载网关', '台', '计划的最大并发。'],
  ['单批下载时长', '秒', '下载完成的预算，不含安装重启。'],
  ['峰值倍率', '倍', '由现场峰值和平均事件率估计。'],
  ['实测平均事件率', '事件/秒', '输入正值时替换估算。0 使用估算。'],
  ['已测稳定转发速率', '事件/秒', '填写同一报文与确认点的实测值。0 表示待压测。']
];
calculator.getRange('A6:F22').values = inputs.map(([label, unit, note], index) => [label, unit, ...plan.scenarios.map((item) => item.values[index]), note]);
calculator.getRange('A6:F22').format.wrapText = true;
calculator.getRange('A6:F22').format.rowHeight = 26;
calculator.getRange('C6:E22').format.fill = '#FFF5DB';
calculator.getRange('A23:F23').values = [['队列配额最大占用比例', '0–1', 0.8, 0.8, 0.8, '0.8 表示队列配额留 20% 余量。']];
calculator.getRange('A23:F23').format.rowHeight = 26;
calculator.getRange('C23:E23').format.fill = '#FFF5DB';
calculator.getRange('A24:F24').values = [['测算结果', '单位', ...plan.scenarios.map((item) => item.name), '解释']];
calculator.getRange('A24:F24').format = headerStyle;
const results = [
  ['心跳平均速率', '次/秒', '管理服务器的平均心跳事件。'],
  ['全量重连均值', '次/秒', '实际握手和鉴权峰值需要压测。'],
  ['业务平均事件率', '事件/秒', '用于预算的聚合后事件。'],
  ['业务峰值事件率', '事件/秒', '整条业务路线的入口目标。'],
  ['本地转发峰值', '事件/秒', '直连场景由客户接收端承接业务流。'],
  ['断链队列估算', 'GB', '单节点单下游，按平均流量和存储开销系数。'],
  ['恢复所需总速率', '事件/秒', '实时按平均流。若恢复期伴随实时峰值，将实时项改为峰值。'],
  ['恢复出站有效载荷', 'Mbps', '仅计本地转发业务数据，另测协议开销和入口流量。'],
  ['OTA 下载有效载荷', 'Mbps', '若与恢复数据共用出口，叠加计算。'],
  ['业务原始日数据量', 'GB/天', '若完整保存一天。客户自行保存或另行核算本地留存。'],
  ['测算与实测速率比较', '状态', '仅比较吞吐目标。还需单独核验延迟、持久化和异常恢复。'],
  ['建议队列配额', 'GB', '估算占用除以目标占用比例，向上取至 0.01 GB。']
];
calculator.getRange('A25:F36').values = results.map(([label, unit, note]) => [label, unit, '', '', '', note]);
calculator.getRange('A25:F36').format.wrapText = true;
calculator.getRange('A25:F36').format.rowHeight = 32;
calculator.getRange('C25:E34').setNumberFormat('#,##0.00');
calculator.getRange('C36:E36').setNumberFormat('#,##0.00');
calculator.getRange('C25:E36').format.fill = '#EDF5FA';
for (const col of ['C', 'D', 'E']) {
  const formulas = [
    `IF(${col}15>0,${col}6/${col}15,0)`,
    `IF(${col}16>0,${col}6/${col}16,0)`,
    `IF(${col}21>0,${col}21,IF(${col}8>0,${col}7/${col}8*${col}9,0))`,
    `${col}27*${col}20`,
    `IF(${col}11=1,${col}28,0)`,
    `IF(${col}11=1,${col}27*${col}12*60*${col}10*${col}14/1000000000,0)`,
    `IF(AND(${col}11=1,${col}13>0),${col}27*(1+${col}12/${col}13),0)`,
    `${col}31*${col}10*8/1000000`,
    `IF(${col}19>0,${col}17*${col}18*1048576*8/${col}19/1000000,0)`,
    `${col}27*${col}10*86400/1000000000`,
    `IF(OR(${col}6<1,${col}7<0,${col}8<=0,${col}9<1,${col}10<=0,AND(${col}11<>0,${col}11<>1),${col}12<0,${col}13<=0,${col}14<1,${col}15<=0,${col}16<=0,${col}17<0,${col}18<0,${col}18>${col}6,${col}19<=0,${col}20<1,${col}21<0,${col}22<0,${col}23<=0,${col}23>1),"核对输入",IF(${col}11=0,"直连验证",IF(${col}22=0,"待压测",IF(${col}22>=MAX(${col}29,${col}31),"达到测算速率","低于测算速率"))))`,
    `IF(${col}23>0,ROUNDUP(${col}30/${col}23,2),0)`
  ];
  calculator.getRange(`${col}25:${col}36`).formulas = formulas.map((formula) => [`=${formula}`]);
}
calculator.getRange('A37').values = [['报表为试算。多目的地、持久化副本、保留历史和恢复期实时峰值需要单独重算。']];
calculator.getRange('A38').values = [['磁盘规格还需容纳系统、日志、备份、升级暂存及剩余空间。']];
calculator.freezePanes.freezeRows(5);

initialize(selection, 'E20', '方案选型与资源起点', '从数据路径和恢复目标定方案。在线网关数用于定义测试范围。', [21, 48, 48, 48, 48]);
addTable(selection, ['方案', '业务形态', '候选测试配置', '关键验证', '升级条件'], [
  [plan.scenarios[0].name, plan.scenarios[0].role, plan.scenarios[0].host, '网关连接、全量重连、命令回执、并发OTA、客户数据确认', '管理API或写入压测达到瓶颈时调整单机资源或管理库'],
  [plan.scenarios[1].name, plan.scenarios[1].role, plan.scenarios[1].host, '峰值、客户端断链、配额、补报、重启及满队列行为', '转发峰值、队列或磁盘 I/O 干扰管理时拆开服务'],
  [plan.scenarios[2].name, plan.scenarios[2].role, plan.scenarios[2].host, '转发节点吞吐与恢复时间，管理端单独验收', '停机目标短于单机恢复时按订单测试冗余与切换']
], 'SizingProfiles', 70);

initialize(acceptance, 'E18', '部署及容量验收补充', '对应 V1.2 的 OH 条目。试点前由客户与交付团队冻结量化通过门槛。', [14, 15, 28, 33, 108]);
addTable(acceptance, ['编号', '阶段', 'V1.2 对应条目', '验收项', '执行及通过标准'], plan.acceptance, 'SizingAcceptance', 55);

workbook.recalculate();
function check(cell, expected) {
  const actual = calculator.getRange(cell).values[0][0];
  if (typeof expected === 'number' ? typeof actual !== 'number' || Math.abs(actual - expected) > 0.00001 : actual !== expected) {
    throw new Error(`${cell}: expected ${expected}, received ${actual}`);
  }
}
for (const [cell, value] of Object.entries({ C25: 1, C27: 300, C30: 0, D27: 1500, D30: 1.62, D31: 3000, D32: 9.6, D36: 2.03, E27: 10000, E30: 27, E31: 20000, E32: 80, E36: 33.75, D35: '待压测' })) check(cell, value);
calculator.getRange('D22').values = [[3100]];
workbook.recalculate();
check('D35', '达到测算速率');
calculator.getRange('D22').values = [[0]];
calculator.getRange('D13').values = [[15]];
workbook.recalculate();
check('D31', 4500);
calculator.getRange('D13').values = [[30]];
workbook.recalculate();
check('D31', 3000);
await fs.mkdir(output, { recursive: true });
const file = await SpreadsheetFile.exportXlsx(workbook);
await file.save(path.join(output, '自有蓝牙网关_容量测算与验收_V1.3.xlsx'));
console.log('Three scenarios, recovery and throughput input recalculation verified');
