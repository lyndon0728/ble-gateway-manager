import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Workbook, SpreadsheetFile } from '@oai/artifact-tool';

const research = path.dirname(fileURLToPath(import.meta.url));
const out = path.resolve(research, '../outputs/ble-platform-onprem-v12');
const read = async (name) => JSON.parse((await fs.readFile(path.join(research, name), 'utf8')).replace(/^\uFEFF/, ''));
const { requirements } = await read('requirements_onprem_v12.json');
const { policies, sources } = await read('onprem_plan_v12.json');
const workbook = Workbook.create();
const overview = workbook.worksheets.add('功能规划');
const detail = workbook.worksheets.add('需求清单');
const handover = workbook.worksheets.add('交付与服务边界');
const evidence = workbook.worksheets.add('参考依据');
const navy = '#203B5C';
const header = { fill: navy, font: { name: 'Microsoft YaHei', size: 10, bold: true, color: '#FFFFFF' }, rowHeight: 30, verticalAlignment: 'center', wrapText: true };
const inputFill = '#FFF5DB';

function initialize(sheet, range, title, note) {
  sheet.showGridLines = false;
  sheet.getRange(range).format.font = { name: 'Microsoft YaHei', size: 10, color: '#1B2B3E' };
  sheet.getRange('A2').values = [[title]];
  sheet.getRange('A2').format.font = { bold: true, size: 15 };
  sheet.getRange('A3').values = [[note]];
  sheet.getRange('A3').format.font = { size: 10, color: '#55677D' };
  sheet.getRange('A2:Q2').format.rowHeight = 28;
  sheet.getRange('A3:Q3').format.rowHeight = 22;
}

function addTable(sheet, headings, rows, widths, name) {
  const range = sheet.getRangeByIndexes(4, 0, rows.length + 1, headings.length);
  range.values = [headings, ...rows];
  range.format.wrapText = true;
  range.format.verticalAlignment = 'top';
  sheet.getRangeByIndexes(4, 0, 1, headings.length).format = header;
  widths.forEach((width, column) => {
    sheet.getRangeByIndexes(0, column, rows.length + 5, 1).format.columnWidth = width;
  });
  rows.forEach((row, index) => {
    const lines = Math.max(...row.map((value, column) => Math.ceil(String(value ?? '').length / (widths[column] * 0.60))));
    sheet.getRangeByIndexes(index + 5, 0, 1, headings.length).format.rowHeight = Math.max(44, lines * 17 + 14);
  });
  sheet.tables.add(range, true, name).showFilterButton = true;
  sheet.freezePanes.freezeRows(5);
}

const end = requirements.length + 5;
const stages = ['P0', 'P1', 'P2'];
const statuses = ['待确认', '已接受', '需修改', '暂缓'];
initialize(overview, 'A1:C42', '自有网关客户服务器版功能规划', 'V1.2  硬件销售  客户局域网部署  客户开发业务应用');
overview.tabColor = navy;
overview.getRange('A1:A42').format.columnWidth = 30;
overview.getRange('B1:B42').format.columnWidth = 20;
overview.getRange('C1:C42').format.columnWidth = 88;
overview.getRange('A5:C38').format.wrapText = true;
overview.getRange('A5:C38').format.verticalAlignment = 'center';
overview.getRange('A5:C38').format.rowHeight = 34;
overview.getRange('A5:C5').values = [['阶段', '需求数', '交付目标']];
overview.getRange('A5:C5').format = header;
overview.getRange('A6:A9').values = [['P0 首期'], ['P1 规模化'], ['P2 订单驱动'], ['合计']];
overview.getRange('B6:B9').formulas = [...stages.map((stage) => [`=COUNTIF('需求清单'!$F$6:$F$${end},"${stage}")`]), ['=SUM(B6:B8)']];
overview.getRange('C6:C9').values = [
  ['断开公网后可安装、注册、收包、管理、升级和恢复。连接型与单上行SKU按条件验收。'],
  ['批量配置、自动灰度、自动备份、故障汇总和维护型 SDK。'],
  ['按客户订单评估高可用、企业 SSO、本地事件推送和离线授权。'],
  ['服务器环境和固件能力在试点前冻结。']
];
overview.getRange('A11:C11').values = [['产品分工', '负责人', '职责']];
overview.getRange('A11:C11').format = header;
overview.getRange('A12:C15').values = [
  ['环境和运行', '客户 IT', '服务器、局域网、证书、维护窗口、备份与恢复。'],
  ['网关日常管理', '客户管理员', '注册、配置、状态、告警、网关固件升级与诊断。'],
  ['业务数据与应用', '客户开发者', '接收、解析、业务存储与应用。使用本地管理 API。'],
  ['安装包与售后', '厂商', '发布签名软件和固件、支持矩阵、交付文档与诊断协助。']
];
overview.getRange('A18:C18').values = [['数据路线', '适用阶段', '设计条件']];
overview.getRange('A18:C18').format = header;
overview.getRange('A19:C21').values = [
  ['网关直达客户接收端', '首期建议', '首销固件支持独立管理和数据目的地，按 MQTT 或 HTTPS 其一验收。'],
  ['客户本地接入转发', '按SKU首期', '验证本地接收、排队和转发，并通过已有管理接口取得命令回执。'],
  ['通用转发与治理', '规模化', '增强多端点配置、积压和回压观测。']
];
overview.getRange('A24:C24').values = [['评审状态', '数量', '评审说明']];
overview.getRange('A24:C24').format = header;
overview.getRange('A25:A28').values = statuses.map((status) => [status]);
overview.getRange('B25:B28').formulas = statuses.map((status) => [`=COUNTIF('需求清单'!$N$6:$N$${end},"${status}")`]);
overview.getRange('C25:C28').values = [['确认型号、协议与现场条件。'], ['冻结验收与负责人。'], ['修改原因填入评审意见。'], ['记录影响并复核依赖。']];
overview.getRange('A31:C31').values = [['交付顺序', '执行者', '验收输出']];
overview.getRange('A31:C31').format = header;
overview.getRange('A32:C37').values = [
  ['预检与离线安装', '客户 IT 与交付', '环境报告、签名发布包、初始管理员及端口证书配置。'],
  ['注册和配置', '客户设备管理员', '已验证设备身份、配置回读与操作记录。'],
  ['客户应用联调', '客户开发者', '本地 API 调用、收首包和命令回执。'],
  ['软件与固件升级', '客户 IT 与管理员', '版本检查、升级记录与回退演练。'],
  ['备份及服务器恢复', '客户 IT', '独立介质备份、换机恢复耗时和缺口记录。'],
  ['售后交接', '客户与厂商', '支持期限、操作手册及诊断包出口权限。']
];
overview.getRange('A40:A42').values = [['建议起步环境为 Linux x86_64 单节点容器部署。'], ['现场验证局域网网络、证书、硬件版本与两条链路。'], ['硬件保修、本地运行权、安装交付和软件维护分别约定。']];

initialize(detail, `A1:Q${end}`, '详细功能需求', '客户服务器部署  自有网关管理  客户应用负责业务处理');
const rows = requirements.map((item) => [item.id, item.type, item.surface, item.domain, item.title, item.priority, item.phase, item.description, item.condition ?? item.notes, item.acceptance, item.dependencies.join(', '), item.persona, item.owner_role, item.status, '', '', item.source_basis]);
addTable(detail, ['需求编号', '类型', '使用入口', '模块', '需求名称', '优先级', '阶段', '详细要求', '适用条件', '验收标准', '前置需求', '用户角色', '责任角色', '评审状态', '实际负责人', '评审意见', '依据属性'], rows, [13, 12, 22, 24, 32, 9, 14, 70, 56, 76, 36, 22, 22, 14, 18, 50, 24], 'OnPremRequirements');
detail.freezePanes.freezeColumns(2);
detail.getRange(`N6:N${end}`).dataValidation = { rule: { type: 'list', values: statuses } };
detail.getRange(`N6:P${end}`).format.fill = inputFill;

initialize(handover, 'A1:F25', '交付与服务边界', '黄色列由产品、交付团队和客户 IT 记录已确认的规格及责任');
addTable(handover, ['事项', '建议政策', '责任角色', '待冻结条件', '正式决定', '确认人'], policies.map((row) => [...row, '', '']), [22, 83, 26, 76, 80, 18], 'OnPremDelivery');
handover.getRange(`E6:F${policies.length + 5}`).format.fill = inputFill;

initialize(evidence, 'A1:F18', '参考依据', '公开资料核验日期 2026年11月7日');
addTable(evidence, ['来源编号', '资料名称', '核验日期', '参考机制', '适用边界', '原始链接'], sources.map((item) => [item.id, item.title, new Date('2026-11-07T00:00:00Z'), item.claim, item.limit, item.url]), [18, 50, 16, 66, 73, 115], 'OnPremSources');
evidence.getRange(`C6:C${sources.length + 5}`).setNumberFormat('yyyy-mm-dd');

workbook.recalculate();
const expectedCounts = stages.map((stage) => requirements.filter((row) => row.priority === stage).length);
const cached = overview.getRange('B6:B9').values.flat();
if (cached.some((value, index) => value !== [...expectedCounts, requirements.length][index])) {
  throw new Error(`Unexpected phase counts: ${JSON.stringify(cached)}`);
}
const previous = detail.getRange('N6').values[0][0];
const pending = overview.getRange('B25').values[0][0];
detail.getRange('N6').values = [['已接受']];
workbook.recalculate();
if (previous !== '待确认' || overview.getRange('B25').values[0][0] !== pending - 1 || overview.getRange('B26').values[0][0] !== 1) {
  throw new Error('Review status calculation failed');
}
detail.getRange('N6').values = [[previous]];
workbook.recalculate();
await fs.mkdir(out, { recursive: true });
const file = await SpreadsheetFile.exportXlsx(workbook);
await file.save(path.join(out, '自有蓝牙网关_客户服务器版_功能需求清单_V1.2.xlsx'));
console.log(JSON.stringify({ requirements: requirements.length, stages: expectedCounts, statusRecalculated: true }));
